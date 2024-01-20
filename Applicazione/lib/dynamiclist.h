typedef struct {
	int len;
	int max_size;
	packet **packets;
} dynamic_list;

void create(dynamic_list *list);
void add(dynamic_list *list, packet *p);
packet *pop(dynamic_list *list);
void remove_el(dynamic_list *list, int index);
void sort(dynamic_list *list);
int contains(dynamic_list list, packet *p);
int is_empty(dynamic_list list);
