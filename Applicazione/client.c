#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "lib/pseudoTCP.h"
#include "lib/dynamiclist.h"

#define PORT 3600

int main() {
	int socket_desc;
	struct sockaddr_in server_addr;
	socklen_t len = sizeof(struct sockaddr_in);
	
	fd_set rset;
	int maxd;
	
	packet *p, *last_pkt;
	struct adaptive_timeout at;
	struct cg_control cg;
	
	dynamic_list list_rcv, list_send;
	
	int expected_num_seq;
	int num_seq;
	int current_ack_rcv;
	
	int pkt_not_acked;
	int ack_retransmission;
	int retransmission_counter;
	int command_acked;
	
	int byte_rcvd;
	int eof;
	int file_size;
	int total_byte_sent;
	int current_byte_read;
	int byte_ready_to_send;
	int i;
	int counter;
	int err;
	
	char buff[1000];
	char line[1000];
	char *token[2];
	
	char path[1000];
	FILE *fp;
	
	struct timeval start, stop;
	
	p = (packet*)malloc(sizeof(packet));
	last_pkt = (packet*)malloc(sizeof(packet));
	
	// Inizializzazione variabili per il timeout adattativo
	at.timeout = TIMEOUT;
	at.estimatedRTT = TIMEOUT;
	at.devRTT = 0;
	
	// Inizializzazione variabili per il controllo di congestione
	cg.cgwin = 1;
	cg.sstresh = 8;
	cg.ack_counter = 0;
	
	// Inizializzo dati
	create(&list_rcv);
	create(&list_send);
	
	expected_num_seq = 0;
	num_seq = 0;
	pkt_not_acked = 0;
	current_ack_rcv = 0;
	retransmission_counter = 0;
	command_acked = 0;
	byte_rcvd = 0;
	eof = 0;
	
	system("clear");

	// Creo la socket 
  if ((socket_desc = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
  	perror("errore creazione socket\n");
  	return -1;
  }
	
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT);
	server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	
	printf("Connessione in corso...\n");

/* ---------------------------------------------- HANDSHAKE ---------------------------------------------- */

	// Preparo il pacchetto SYN
	p = make_pkt(num_seq, 0, 0, 0, 0, 0, 0, 1, "");
	
	num_seq += 1; 
	
	// Invio comando al server
	start_timer(p);
	send_pkt(socket_desc, server_addr, p);
	
	// Salvo il pacchetto per ritrasmetterlo in caso di perdita
	copy_pkt(last_pkt, p);
	
	// Finchè non ricevo ACK rimango in attesa
	gettimeofday(&start, NULL);
	while (1) {
		gettimeofday(&stop, NULL);
		// Dopo un certo periodo di inattività deduco che il server non è disponibile
		if (stop.tv_sec - start.tv_sec >= WAIT) {
			printf("Il server non risponde. Chiusura\n");
			
			// Dealloco le risorse e chiudo i descrittori
			free(p);
			free(last_pkt);
			close(socket_desc);
			exit(0);
		}
		
		check_timeout(socket_desc, server_addr, last_pkt, &at, &cg);
	
		if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&server_addr, &len) < 0) {
			continue;
		}
		
		// Resetto il timer
		gettimeofday(&start, NULL);
		
		if (p->ack == 1 && p->command == 1 && p->num_ack > current_ack_rcv) {
			current_ack_rcv = p->num_ack;
			expected_num_seq++;
				
			// Invio ACK
			p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, 0, 0, "");
			
			send_pkt(socket_desc, server_addr, p);
			copy_pkt(last_pkt, p);
			break;
		}
	}

/* ---------------------------------------------- HANDSHAKE ---------------------------------------------- */

	system("clear");
	
	FD_ZERO(&rset);

	while (1) {
		FD_SET(fileno(stdin), &rset);
		FD_SET(socket_desc, &rset);
		maxd = (socket_desc > fileno(stdin)) ? (socket_desc+1) : (fileno(stdin)+1);
		
		if (select(maxd, &rset, NULL, NULL, NULL) < 0) {
			perror("errore in select");
			return -1;
		} 
	
		memset(line, '\0', sizeof(line));
		
		// Ascolto socket nel caso riceva comando di chiusura oppure pacchetti gia ricevuti ma non riscontrati dal server
		if (FD_ISSET(socket_desc, &rset)) {
			if (recvfrom(socket_desc, p, sizeof(packet), 0, (struct sockaddr*)&server_addr, &len) < 0) {
				perror("errore in socket d'ascolto");
				return -1;
			}
			
			// ricezione comando di chiusura dal server
			if (p->command == 2) {
				printf("Chiusura in corso...\n");
				
				expected_num_seq++;
				
				// Invio ACK
				p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, 0, 0, "");
				
				send_pkt(socket_desc, server_addr, p);
				copy_pkt(last_pkt, p);
				
				// Attesa temporizzata
				gettimeofday(&start, NULL);
				while (1) {
					gettimeofday(&stop, NULL);
					
					// se non ricevo nulla per un certo tempo, chiudo
					if (stop.tv_sec - start.tv_sec >= WAIT) {
						printf("Chiusura\n");
						break;
					}
					
					// ascolto pacchetti in arrivo
					if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&server_addr, &len) < 0) {
						continue;
					}
					
					// Resetto il timer
					gettimeofday(&start, NULL);
					
					// Scarto i pacchetti non relativi al comando di chiusura
					if (p->command != 2) {
						continue;
					}
					
					// Ritrasmetto
					send_pkt(socket_desc, server_addr, last_pkt);
				}
				
				break;
			}
			
			// ricezione comando di errore dal server
			if (p->command > 5 && p->num_seq == expected_num_seq) {
				printf("Errore di paginazione nel server. Chiusura\n");
				
				expected_num_seq++;
				
				// Invio ACK
				p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, 0, 0, "");
				
				send_pkt(socket_desc, server_addr, p);
				copy_pkt(last_pkt, p);
				
				// Attesa temporizzata
				gettimeofday(&start, NULL);
				while (1) {
					gettimeofday(&stop, NULL);
					
					// se non ricevo nulla per un certo tempo, chiudo
					if (stop.tv_sec - start.tv_sec >= WAIT) {
						printf("Chiusura\n");
						break;
					}
					
					// ascolto pacchetti in arrivo
					if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&server_addr, &len) < 0) {
						continue;
					}
					
					// Resetto il timer
					gettimeofday(&start, NULL);
					
					// Scarto i pacchetti non relativi al comando di errore
					if (p->command < 6) {
						continue;
					}

					// Ritrasmetto
					send_pkt(socket_desc, server_addr, last_pkt);
				}
				
				break;
			}
			
			// Ricezione pacchetti in ritardo già ricevuti (ad esempio dopo get o list)
			if (p->num_seq < expected_num_seq && p->command < 6) {
				// ritrasmetto ack duplicato
				send_pkt(socket_desc, server_addr, last_pkt);
			}
			
			// tutti gli altri casi sono pacchetti non desiderati, che scarto
			continue;
		}
		
		// Ascolto anche lo stdin per ricevere comandi dall'utente
		if (FD_ISSET(fileno(stdin), &rset)) {
			if (fgets(line, sizeof(line), stdin) < 0) {
				perror("errore in fgets");
				return -1;
			}
		}
		
		// Se ricevo una stringa vuota non la processo
		if (strcmp(line, "\n") == 0) {
			continue;
		}
		
		// Tokenizzo il comando ricevuto
		token[0] = strtok(line, " \n");
		token[1] = strtok(NULL, " \n");
		
		// Scarto tutto ciò che ricevo oltre il 2 token
		
/* ------------------------------------------------ HELP ------------------------------------------------- */
		if (strcmp(token[0], "help") == 0) {
			
			fp = fopen("lib/help", "r");
			if (fp == NULL) {
				perror("errore apertura file help");
			}
			
			fread(buff, sizeof(char), sizeof(buff), fp);
			
			printf("%s\n", buff);
			
			fclose(fp);
			continue;
		}
/* ------------------------------------------------ HELP ------------------------------------------------- */
		
/* ------------------------------------------------ LIST ------------------------------------------------- */
		if (strcmp(token[0], "list") == 0) {
			// Preparo il pacchetto contenente il comando da inviare al server
			
			if (token[1] == NULL) {
				p = make_pkt(num_seq, 0, 0, 0, 0, 0, 0, 3, "");
			} else {
				p = make_pkt(num_seq, 0, 0, 0, 0, strlen(token[1]), 0, 3, token[1]);
			}
			
			// Invio comando al server
			start_timer(p);
			send_pkt(socket_desc, server_addr, p);
			copy_pkt(last_pkt, p);
			
			command_acked = 0;
			eof = 0;
			byte_rcvd = 0;
			gettimeofday(&start, NULL);
			while (!eof) {
				gettimeofday(&stop, NULL);
				// Dopo un certo periodo di inattività deduco che la connessione si è interrotta
				if (stop.tv_sec - start.tv_sec >= WAIT) {
					perror("Server non risponde. Chiusura");
					
					// Dealloco le risorse e chiudo i descrittori
					free(p);
					free(last_pkt);
					close(socket_desc);
					exit(0);
				}
				
				// Se il pacchetto contenente il comando va in timeout lo ritrasmetto
				if (!command_acked) {
					check_timeout(socket_desc, server_addr, last_pkt, &at, &cg);
				}
				
				memset(p->data, '\0', sizeof(p->data));
				
				// Ascolto (non bloccante)
				if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&server_addr, &len) < 0) {
					continue;
				}
				
				// Resetto il timer
				gettimeofday(&start, NULL);

				// Ricezione di ack con errore
				if (p->command > 5) {
					
					// Discrimino la tipologia d'errore avvenuto per stamparlo a schermo
					if (p->command == 6) {
						printf("errore: apertura file o directory, parametro non valido o pagina inesistente\n");
					} else if (p->command == 7) {
						printf("errore: dimensione file troppo grande. Specificare la pagina che si desidera visualizzare\n");
					}
					
					current_ack_rcv = p->num_ack;
					num_seq += (token[1] != NULL ? strlen(token[1]) : 1);
					
					printf("Operazione interrotta\n");
					break;
				}
								
				// Se il pacchetto ha num_seq < expected_num_seq lo scarto e invio direttamente ACK duplicato
				
				// Se il pacchetto ha num_seq >= expected_num_seq e non è gia stato ricevuto lo metto in lista di ricezione
				if (p->num_seq >= expected_num_seq && !contains(list_rcv, p)) {
					add(&list_rcv, p);
					sort(&list_rcv);
				}
				
				// Se ricevo ACK valido, posso processare list_rcv
				if (p->ack == 1 && !command_acked) {
					current_ack_rcv = p->num_ack;
					num_seq += (token[1] != NULL ? strlen(token[1]) : 1);
					command_acked = 1;
				}
				
				// Finchè non ricevo ACK, non posso processare list_rcv
				if (!command_acked) {
					continue;
				}
				
				// Se il pacchetto ha num_seq == expected_num_seq scorro in lista e rimuovo tutti gli eventuali pacchetti contigui
				if (p->num_seq == expected_num_seq) {
					ack_retransmission = list_rcv.packets[0]->n_retransmission;
					while (!is_empty(list_rcv) && list_rcv.packets[0]->num_seq == expected_num_seq) {
						p = pop(&list_rcv);
						expected_num_seq += p->pkt_size;
						
						// Stampo a schermo il risultato parziale dell'operazione facendo uso di un buffer ausiliario
						// Uso fwrite per stampare esattamente p->pkt_size byte, cosi non mi preoccupo del terminatore
						fwrite(p->data, sizeof(char), p->pkt_size, stdout);
						
						byte_rcvd += p->pkt_size;
						
						if (p->file_size - byte_rcvd == 0) {
							eof = 1;
						}
					}
				}
				
				// Invio ACK
				p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, ack_retransmission, 0, "");
				send_pkt(socket_desc, server_addr, p);
			}
			
			printf("Operazione terminata\n");
			
			// metto dentro last_pkt l'ultimo ack che ho inviato
			copy_pkt(last_pkt, p);
			continue;
		}
/* ------------------------------------------------ LIST ------------------------------------------------- */
		
/* ------------------------------------------------ GET -------------------------------------------------- */
		if (strcmp(token[0], "get") == 0) {
			if (token[1] == NULL) {
				perror("la funzione get richiede un parametro\n");
				continue;
			}
			
			printf("Operazione in corso...\n");
			
			sprintf(path, "clientfiles/%s", token[1]);
		
			// Per prima cosa tento di aprire il file per vedere se posso procedere al download
			if ((fp = fopen(path, "ab")) == NULL) {
				perror("errore creazione file");
				continue;
			}
			
			// Preparo il pacchetto contenente il comando da inviare al server
			p = make_pkt(num_seq, 0, 0, 0, 0, strlen(token[1]), 0, 4, token[1]);
			
			// Invio comando al server
			start_timer(p);
			send_pkt(socket_desc, server_addr, p);
			
			copy_pkt(last_pkt, p);
			
			command_acked = 0;
			eof = 0;
			byte_rcvd = 0;
			gettimeofday(&start, NULL);
			while (!eof) {
				gettimeofday(&stop, NULL);
				// Dopo un certo periodo di inattività deduco che la connessione si è interrotta
				if (stop.tv_sec - start.tv_sec >= WAIT) {
					perror("Server non risponde. Chiusura");
					
					// Dealloco le risorse e chiudo i descrittori
					free(p);
					free(last_pkt);
					close(socket_desc);
					exit(0);
				}
				
				// Se il pacchetto contenente il comando va in timeout lo ritrasmetto
				if (!command_acked) {
					check_timeout(socket_desc, server_addr, last_pkt, &at, &cg);
				}
				
				memset(p->data, '\0', sizeof(p->data));
			
				// Ascolto
				if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&server_addr, &len) < 0) {
					continue;
				}
				
				// Resetto il timer
				gettimeofday(&start, NULL);
				
				// Ricezione di errore
				if (p->command > 5) {
					// Discrimino la tipologia d'errore avvenuto per stamparlo a schermo
					if (p->command == 6) {
						printf("errore: apertura file o directory\n");
					} else if (p->command == 7) {
						printf("errore: dimensione file troppo grande (limite massimo 1 GB)\n");
					}
					
					current_ack_rcv = p->num_ack;
					num_seq += strlen(token[1]);
					
					// elimino il file aperto 
					if (remove(path) != 0) {
						perror("errore cancellazione file");
					}
					
					printf("Download interrotto\n");
					
					break;
				}
				
				// Se il pacchetto ha num_seq < expected_num_seq lo scarto e invio direttamente ACK duplicato
				
				// Se il pacchetto ha num_seq >= expected_num_seq lo metto in lista di ricezione
				if (p->num_seq >= expected_num_seq && !contains(list_rcv, p)) {
					add(&list_rcv, p);
					sort(&list_rcv);
				}
				
				// Se ricevo ACK, posso processare list_rcv
				if (p->ack == 1 && !command_acked) {
					command_acked = 1;
					current_ack_rcv = p->num_ack;
					num_seq += strlen(token[1]);
				}
				
				// Finchè non ricevo ACK, non posso processare list_rcv
				if (!command_acked) {
					continue;
				}
				
				// Se il pacchetto ha num_seq == expected_num_seq scorro in lista e riscontro tutti gli eventuali pacchetti contigui
				if (p->num_seq == expected_num_seq) {	
					ack_retransmission = list_rcv.packets[0]->n_retransmission;
					while (!is_empty(list_rcv) && list_rcv.packets[0]->num_seq == expected_num_seq) {
						p = pop(&list_rcv);
						expected_num_seq += p->pkt_size;
						
						// Copio il contenuto del pacchetto in un file locale
						fwrite(p->data, sizeof(char), p->pkt_size, fp);
						
						byte_rcvd += p->pkt_size;
						
						if (p->file_size - byte_rcvd == 0) {
							eof = 1;
						}
					}
				}
				
				// Invio ACK
				p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, ack_retransmission, 0, "");
				
				send_pkt(socket_desc, server_addr, p);
			}
			
			fclose(fp);
			
			copy_pkt(last_pkt, p);
			
			printf("Operazione terminata\n");
			continue;
		}
/* ------------------------------------------------ GET -------------------------------------------------- */
		
/* ------------------------------------------------ PUT -------------------------------------------------- */
		if (strcmp(token[0], "put") == 0) {
			
			if (token[1] == NULL) {
				perror("la funzione put richiede un parametro\n");
				continue;
			}
			
			printf("Operazione in corso...\n");
			
			sprintf(path, "clientfiles/%s", token[1]);
			
			// apro e leggo il file 
			if ((fp = fopen(path, "rb")) == NULL) {
				perror("errore apertura file");
				continue;
			}
			
			// calcolo preliminarmente dimensione file e la metto nel campo del pacchetto
			fseek(fp, 0, SEEK_END);
			file_size = ftell(fp);
			rewind(fp);
			
			// Limite sulla dimensione massima di un file inviato
			if (file_size > 1000000000) {
				perror("errore: file troppo grande (dimensione massima 1 GB)");
				fclose(fp);
				continue;
			}
			
			// In testa a tutti c'è il pacchetto contenente il comando e il nome del file
			p = make_pkt(num_seq, 0, 0, 0, file_size, strlen(token[1]), 0, 5, token[1]);
			
			// Invio il pacchetto
			start_timer(p);
			send_pkt(socket_desc, server_addr, p);
			
			// Lo salvo per ritrasmetterlo in caso di perdita
			copy_pkt(last_pkt, p);

			// Attendo ACK prima di continuare perchè potrei ricevere un messaggio d'errore
			err = 0;
			gettimeofday(&start, NULL);
			while (1) {
				gettimeofday(&stop, NULL);
				// Dopo un certo periodo di inattività deduco che la connessione si è interrotta 
				if (stop.tv_sec - start.tv_sec >= WAIT) {
					perror("Server non risponde. Chiusura");
					
					// Dealloco le risorse e chiudo i descrittori
					free(p);
					free(last_pkt);
					close(socket_desc);
					exit(0);
				}
				
				check_timeout(socket_desc, server_addr, last_pkt, &at, &cg);
				
				if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&server_addr, &len) < 0) {
					continue;
				}
								
				// Resetto il timer
				gettimeofday(&start, NULL);
					
				if (p->ack == 1 && p->num_ack > current_ack_rcv) {
					current_ack_rcv = p->num_ack;
					num_seq += strlen(token[1]);
					
					// Ricezione errore 6
					if (p->command == 6) {
						printf("errore: apertura file o directory\n");
						err = 1;
					}
					
					// Ricezione errore 7
					if (p->command == 7) {
						printf("errore: dimensione file troppo grande (limite massimo 1 GB)\n");
						err = 1;
					}
				}
					
				break;
			}

			// Se ho ricevuto errore, mi fermo
			if (err) {
				printf("Operazione interrotta\n");
				continue;
			}

			// Se sono qui non ho ricevuto errori e procedo con la put
			
			total_byte_sent = 0;
			while (total_byte_sent < file_size) {
				p->ack = 0;
				p->num_ack = 0;
				p->timer = 0;
				p->command = 5;
				memset(p->data, '\0', sizeof(p->data));
			
				// Leggo 1000 caratteri per volta, e riempio la lista 
				byte_ready_to_send = 0;
				while (list_send.len < list_send.max_size-1 && (current_byte_read = fread(p->data, sizeof(char), sizeof(p->data), fp)) > 0) {
					
					p->num_seq = num_seq;
					
					// Aggiorno num_seq
					num_seq += current_byte_read;
					
					byte_ready_to_send += current_byte_read;
					
					p->pkt_size = current_byte_read;
					
					p->n_retransmission = 0;
					
					// Inserisco il pacchetto in lista
					add(&list_send, p);
					
					// Ripulisco il campo data
					memset(p->data, '\0', sizeof(p->data));
				}	
				
				// Quando la lista è riempita, invio i pacchetti
				gettimeofday(&start, NULL);
				while (!is_empty(list_send)) {
					gettimeofday(&stop, NULL);
					
					// Dopo un certo periodo di inattività deduco che la connessione si è interrotta
					if (stop.tv_sec - start.tv_sec >= WAIT) {
						perror("Server non risponde. Chiusura");
						
						// Dealloco le risorse e chiudo i descrittori
						free(p);
						free(last_pkt);
						close(socket_desc);
						exit(0);
					}
						
					// Ritrasmetto pacchetti in timeout
					for (int i=0;i<pkt_not_acked;i++) {
						check_timeout(socket_desc, server_addr, list_send.packets[i], &at, &cg);
					}
					
					// Invio pacchetto
					if (pkt_not_acked < list_send.len && pkt_not_acked < cg.cgwin) {
						start_timer(list_send.packets[pkt_not_acked]);
						send_pkt(socket_desc, server_addr, list_send.packets[pkt_not_acked]);
						pkt_not_acked++;
					}
				
					// Se non ci sono pacchetti inviati e non riscontrati, riparto
					if (pkt_not_acked == 0) {
						continue;
					}
					
					// Ascolto pacchetti in modalità non bloccante
					if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&server_addr, &len) < 0) {
						continue;
					}
					
					// Resetto il timer
					gettimeofday(&stop, NULL);
					
					// Ricezione di errore di paginazione al termine della put
					// Questo serve nel caso in cui il client non abbia ancora ricevuto l'ultimo ACK dal server
					if (p->command == 6) {
						perror("Errore di paginazione nel server. Chiusura");
						
						// Forzo il valore di expected_num_seq
						expected_num_seq = p->num_seq + 1;
						
						// Invio ACK
						p = make_pkt(0, 1, expected_num_seq, 0, 0, 0, 0, 0, "");
						send_pkt(socket_desc, server_addr, p);
						
						// Attesa temporizzata
						gettimeofday(&start, NULL);
						while (1) {
							gettimeofday(&stop, NULL);
							
							// se non ricevo nulla per un certo tempo, chiudo
							if (stop.tv_sec - start.tv_sec >= WAIT) {
								printf("Chiusura\n");
								break;
							}
							
							// ascolto pacchetti in arrivo
							if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&server_addr, &len) < 0) {
								continue;
							}
							
							// Resetto il timer
							gettimeofday(&start, NULL);
							
							// Scarto i pacchetti non relativi al comando di errore
							if (p->command < 6) {
								continue;
							}

							// Ritrasmetto
							send_pkt(socket_desc, server_addr, last_pkt);
						}
						
						// Dealloco tutte le risorse
						free(p);
						free(last_pkt);
						fclose(fp);
						close(socket_desc);
						exit(0);
					}
						
					// Controllo su pacchetti non validi
					if (p->ack != 1 || p->num_ack < current_ack_rcv) {
						continue;
					}
					
					// Ritrasmissione rapida in caso di ack duplicati
					if (p->num_ack == current_ack_rcv) {
						quick_retransmission(socket_desc, server_addr, list_send.packets[0], &retransmission_counter);
					} 
					
					// Ack validi
					if (p->num_ack > current_ack_rcv) {
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
			}

			fclose(fp);
			printf("Fine\n");
			continue;
		}
/* ------------------------------------------------ PUT -------------------------------------------------- */
		
/* ------------------------------------------------ FIN -------------------------------------------------- */
		if (strcmp(token[0], "quit") == 0) {
			// Preparo pacchetto FIN
			p = make_pkt(num_seq, 0, 0, 0, 0, 0, 0, 2, "");
			
			// Invio comando FIN al server
			start_timer(p);
			send_pkt(socket_desc, server_addr, p);
			
			// Salvo il pacchetto per ritrasmetterlo in caso di perdita
			copy_pkt(last_pkt, p);
			
			// Finchè non ricevo ACK rimango in attesa
			gettimeofday(&start, NULL);
			while (1) {
				gettimeofday(&stop, NULL);
				// Dopo un certo periodo di inattività deduco che la connessione si è interrotta
				if (stop.tv_sec - start.tv_sec >= WAIT) {
					printf("Il server non risponde. Chiusura\n");
					break;
				}
				
				check_timeout(socket_desc, server_addr, last_pkt, &at, &cg);
			
				if (recvfrom(socket_desc, p, sizeof(packet), MSG_DONTWAIT, (struct sockaddr*)&server_addr, &len) < 0) {
					continue;
				}
				
				// Resetto il timer
				gettimeofday(&start, NULL);
				
				if (p->ack == 1 && p->num_ack > current_ack_rcv) {
					printf("Chiusura\n");
					break;
				}
			}
			
			break;
		}
/* ------------------------------------------------ FIN -------------------------------------------------- */
		
		printf("Comando non riconosciuto\n");
	}
	
	free(p);
	free(last_pkt);
	close(socket_desc);
	exit(0);
}
