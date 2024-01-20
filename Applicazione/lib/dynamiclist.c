#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pseudoTCP.h"
#include "dynamiclist.h"

void create(dynamic_list *list) {
	// Inizializzo una lista vuota
	list->len = 0;
	list->max_size = 100;
	list->packets = NULL;
}

void add(dynamic_list *list, packet *old_p) {	
	packet **temp_packets;
	packet *new_p;
	
	// Creo una copia del segmento da aggiungere
	new_p = (packet*)malloc(sizeof(packet));
	
	copy_pkt(new_p, old_p);
	
	// Alloco lo spazio per una nuova lista da sostituire alla precedente
	temp_packets = (packet**)malloc((list->len + 1) * sizeof(packet*));
	
	// Copio nella nuova lista tutti gli elementi della vecchia lista, aggiungendo in ultima posizione il nuovo elemento
	for (int i=0;i<list->len;i++) {
		temp_packets[i] = (packet*)malloc(sizeof(packet));
		temp_packets[i] = list->packets[i];
	}
	temp_packets[list->len] = (packet*)malloc(sizeof(packet));
	temp_packets[list->len] = new_p;
	
	// Libero lo spazio utilizzato dalla vecchia lista, sostituisco la vecchia lista con la nuova, e aggiorno la dimensione
	free(list->packets);
	list->packets = temp_packets;
	list->len++;
}

packet *pop(dynamic_list *list) {
	packet **temp_packets;
	packet *p;
	
	// Se la lista è vuota, ritorno NULL
	if (list->len == 0) {
		printf("Lista vuota\n");
		return NULL;
	}
	
	temp_packets = NULL;
	
	// Prelevo l'elemento in cima alla lista
	p = list->packets[0];
	
	// Se la vecchia lista ha dimensione maggiore di 1, creo una copia della vecchia lista shiftata a sinistra di un elemento
	if (list->len > 1) {
		temp_packets = (packet**)malloc((list->len - 1) * sizeof(packet*));
		
		for (int i=0;i<list->len-1;i++) {
			temp_packets[i] = (packet*)malloc(sizeof(packet));
			temp_packets[i] = list->packets[i+1];
		}
	}
	
	// Libero lo spazio utilizzato dalla vecchia lista, sostituisco la vecchia lista con la nuova, e aggiorno la dimensione
	free(list->packets);
	list->packets = temp_packets;
	list->len--;
	
	// Ritorno l'elemento estratto
	return p;
}

void remove_el(dynamic_list *list, int index) {
	packet **temp_packets;
	int k = 0;
	
	if (list->len == 0) {
		perror("Lista vuota\n");
		return;
	}
	
	if (index > list->len) {
		perror("Indice più grande della dimensione della lista\n");
		return;
	}
	
	// Alloco lo spazio per una nuova lista
	temp_packets = (packet**)malloc((list->len - 1) * sizeof(packet*));
	for (int i=0;i<list->len;i++) {
		if (i == index) {
			continue;
		}
		
		temp_packets[k] = (packet*)malloc(sizeof(packet));
		
		// Inserisco nella nuova lista tutti gli elementi della vecchia, tranne quello in posizione index
		temp_packets[k] = list->packets[i];
		k++;
	}
	
	list->packets = temp_packets;
	list->len--;
}

// Based on num_seq
void sort(dynamic_list *list) {
	dynamic_list new_list;
	int local_min = 0, index = 0;
	
	// Alloco lo spazio per una nuova lista da sostituire alla precedente
	create(&new_list);
	
	// Creo una nuova list ordinata
	while (!is_empty(*list)) {	
	
		// calcolo il minimo parziale
		local_min = list->packets[0]->num_seq;
		for (int j=0;j<list->len;j++) {
			if (list->packets[j]->num_seq <= local_min) {
				local_min = list->packets[j]->num_seq;
				index = j;
			}
		}
		
		// Aggiungo il segmento nella nuova lista
		add(&new_list, list->packets[index]);
		
		// Rimuovo il segmento in posizione index dalla vecchia lista
		remove_el(list, index);
	}
	
	list->packets = new_list.packets;
	list->len = new_list.len;
}

int contains(dynamic_list list, packet *p) {
	for (int i=0;i<list.len;i++) {
		if (list.packets[i]->num_seq == p->num_seq) {
			return 1;
		}
	}
	return 0;
}

int is_empty(dynamic_list list) {
	if (list.len == 0) {
		return 1;
	}
	return 0;
}
