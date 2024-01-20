#define MAX_WINDOW 20
#define MAX_PKT_SIZE 1000

typedef struct {
	unsigned long long num_seq;
	unsigned long long num_ack;
	unsigned int ack : 1;
	unsigned long long timer;
	unsigned int file_size;
	unsigned int pkt_size : 12;
	unsigned short n_retransmission;
	unsigned int command : 3;
	char data[MAX_PKT_SIZE];
} packet;

struct cg_control{
	int cgwin;
	int sstresh;
	int ack_counter;
};

struct adaptive_timeout{
	int timeout;
	int estimatedRTT;
	int devRTT;
};

struct sockaddr_in;

void start_timer(packet *p);
unsigned long get_timer(packet *p);
int check_timeout(int socket_desc, struct sockaddr_in addr, packet *p, struct adaptive_timeout *at, struct cg_control *cg);
int update_timeout(struct adaptive_timeout *at, packet *p, int current_ack_rcv, int ack_retransmission);
int reset_cgwin(struct cg_control *cg);
int increase_cgwin(struct cg_control *cg);
packet *make_pkt(unsigned long long num_seq, unsigned int ack, unsigned long long num_ack, unsigned long long timer, unsigned int file_size, unsigned int pkt_size, unsigned short n_retransmission, unsigned int command, char *data);
int send_pkt(int socket_desc, struct sockaddr_in receiver_addr, packet *p);
int copy_pkt(packet *dest, packet *src);
int quick_retransmission(int socket_desc, struct sockaddr_in addr, packet *p, int *retransmission_counter);
