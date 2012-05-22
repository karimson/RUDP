#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "event.h"
#include "rudp.h"
#include "rudp_api.h"

typedef struct 
{
    struct rudp_hdr header;
    char    data[RUDP_MAXPKTSIZE];
}__attribute__((packed)) rudp_packet_t;

/* Structure for outgoing packet buffer */
struct send_packet_structure
{
    int send_flag;                          // 1: packet sent, 0: not yet sent
    rudp_packet_t rudp_packet;              // data to be sent
    int len;                                // data length
    int transcnt;                   // transmission counter
    struct send_packet_structure *next_buff;     // pointer to the next buffer area
};

/* Structure for send window */
struct send_data_window
{
    int send_flag;                          // 1: packet sent, 0: not yet sent
    rudp_packet_t *rudp_packet;             // data to be sent
    int len;                                // data length
    struct send_data_window *next_buff;     // pointer to the next buffer area
};

/* Structure for list of peers to send data to */
struct rudp_client
{
    int status;                             // 1:initial, 2:sending DATA, 3:closing socket
    struct sockaddr_in rsock_addr;          // the socket address of the destination
    unsigned int seq;                       // sequence number
    struct send_packet_structure *queue_buff;    // outgoing data buffer
    struct send_data_window *window;        // window buffer
};

/* Structure for list of peers to receive data from */
/*struct rudp_recv_peer
{
    int status;                     // 
    struct sockaddr_in rsock_addr;  // the socket address of the incoming peer
    unsigned int last_seq;                  // sequence number
};*/

struct rudp_socket_type
{
    int socketfd;
    struct sockaddr_in rsock_addr;
    struct rudp_client *outgoing_peer;   // send peer
    struct rudp_client *incoming_peer;   // receive peer
    int (*recvfrom_handler_callback)(rudp_socket_t, struct sockaddr_in *, char *, int);
    int (*event_handler_callback)(rudp_socket_t, rudp_event_t, struct sockaddr_in *);
};

struct rudp_socket_type *global_socket = NULL;

bool rudp_fin_received = FALSE;
bool dropped = FALSE;

int rudp_receive_data(int fd, void *arg);
void rudp_process_received_packet(void *buf, struct rudp_socket_type *rsocket, int len);
int rudp_send_ack_packet(struct rudp_socket_type *rsocket, struct sockaddr_in *from, unsigned int seq_num);
int transmit(struct rudp_client *send_peer, int sockfd);
int rudp_send_data(struct rudp_socket_type *rsocket, struct sockaddr_in *from, unsigned int seq);
int rudp_send_data_ack(struct rudp_socket_type *rsocket, struct sockaddr_in *to, unsigned int seq_num, char *data, int len);
int retransmit(int fd, void *arg);
int rudp_process_fin_msg(struct rudp_socket_type *rsocket, struct sockaddr_in *to, unsigned int seq_num);

int rudp_send_data(struct rudp_socket_type *rsocket, struct sockaddr_in *from, unsigned int seq)
{
    struct rudp_socket_type *rsock;         // pointer to open socket
    struct rudp_client *send_peer;       // pointer to send peer list
    struct send_data_window *window;        // pointer to send window
    struct send_packet_structure *packet_buff;   // pointer to packet buffer
    unsigned int seqno;                     // packet sequence number
    int sockfd; // dummy variables
    
    // check whether the socket is in the open socket list
    rsock = rsocket;
    send_peer = rsock->outgoing_peer;
    
    // check the sequence number, remove data with smaller sequence number from window
    window = send_peer->window;
    packet_buff = send_peer->queue_buff;
        
    while(window != NULL)
    {
        seqno = window->rudp_packet->header.seqno;
        
        if(SEQ_LT(seqno, seq))
        {
            fprintf(stderr, "LESS THAN: Seqno: %0x Seq: %0x\n", seqno, seq);
            // delete timeout register
            if(event_timeout_delete(retransmit, send_peer->window->rudp_packet) == -1)
            {
                perror("rudp: Error deleting timeout..\n");
                return -1;
            }
            
            // next node
            send_peer->window = window->next_buff;
            send_peer->queue_buff = packet_buff->next_buff;
            
            free(window);
            free(packet_buff);
            window = send_peer->window;
            packet_buff = send_peer->queue_buff;
        }
        else
        {
            fprintf(stderr, "ELSE: Seqno: %0x Seq: %0x\n", seqno, seq);
            window = window->next_buff;
            packet_buff = packet_buff->next_buff;
        }
    }
            
    // check whether the peer status is FINISHED
    if(send_peer->status == FINISHED && send_peer->window == NULL)
    {
        rsock->outgoing_peer = NULL;
        
        printf("rudp: Signaling RUDP_EVENT_CLOSED to application\n");
        rsock->event_handler_callback((rudp_socket_t)rsock, RUDP_EVENT_CLOSED, NULL);
        
        // if the incoming peer list is also empty, close the socket
        if(rsock->incoming_peer == NULL)
        {
            sockfd = rsock->socketfd;
            
            // remove socket from open socket list
            event_fd_delete(rudp_receive_data, (void *)rsock);
            printf("rudp: Closing socket %d\n", sockfd);
            close(sockfd);
        }
        
        return 0;
    }
            
    // if not, continue transmission to the peer
    // printf("rudp: Debug: Continue transmission.\n");
    transmit(send_peer, rsock->socketfd);
    return 0;
       
}
/*===================================================================================
 * Send an ACK Packet to the Sender
 *===================================================================================*/
int rudp_send_data_ack(struct rudp_socket_type *rsocket, struct sockaddr_in *to, unsigned int seq_num, char *data, int len)
{
    rudp_packet_t rudp_ack;
    struct rudp_socket_type *socket;
    struct rudp_client *recv_peer;
    int ret = 0;
    
    recv_peer = NULL;
    
    socket = global_socket;
    recv_peer = socket->incoming_peer;
 
    if(recv_peer != NULL)
    {
        if(seq_num == recv_peer->seq+1) 
        {
            if(recv_peer->status != FINISHED) 
            {
                rsocket->recvfrom_handler_callback(rsocket, to, data, len);     
                recv_peer->seq++;
            }
            else
            {
                printf("Receiver socket is closed. Now packets will be dropped.\n");
                return -1;
            }
        }
        else
        {
            printf("[ERROR] Packet (%d) is dropped. Last packet is (%d).\n", recv_peer->seq+1, recv_peer->seq);
        }
    }
    else
    {
        printf("Received from Unknown sender or None SYN.\n");
        return -1;
    }
    /////////////////////////////////////////////////////////
    // Send ACK packet for received DATA packet
    /////////////////////////////////////////////////////////       
    memset(&rudp_ack, 0x0, sizeof(rudp_packet_t));
    
    rudp_ack.header.version = RUDP_VERSION;
    rudp_ack.header.type = RUDP_ACK;
    //rudp_ack.header.seqno = seq_num;
    rudp_ack.header.seqno = recv_peer->seq+1;
    
    ret = sendto((int)rsocket->socketfd, (void *)&rudp_ack, sizeof(rudp_packet_t), 0,
                 (struct sockaddr*)to, sizeof(struct sockaddr_in));
    if(ret <= 0)
    {
        fprintf(stderr, "rudp: sendto fail(%d)\n", ret);
        return -1;
    }
    
    printf("[Data ACK] (seq: %d) to the Sender.\n", rudp_ack.header.seqno);
    return 0;
}
/*===================================================================================
 * Send an ACK Packet to the Sender
 *===================================================================================*/
int rudp_send_ack_packet(struct rudp_socket_type *rsocket, struct sockaddr_in *to, unsigned int seq_num)
{
    rudp_packet_t rudp_ack;
    struct rudp_socket_type *socket;
    struct rudp_client *recv_peer, *new_recv_peer, *prev_recv_peer;
    int ret = 0;
    
    recv_peer = new_recv_peer = prev_recv_peer = NULL;
    
    memset(&rudp_ack, 0x0, sizeof(rudp_packet_t));
    
    rudp_ack.header.version = RUDP_VERSION;
    rudp_ack.header.type = RUDP_ACK;
    rudp_ack.header.seqno = seq_num;
    
    ret = sendto((int)rsocket->socketfd, (void *)&rudp_ack, sizeof(rudp_packet_t), 0,
                 (struct sockaddr*)to, sizeof(struct sockaddr_in));
    if(ret <= 0)
    {
        fprintf(stderr, "rudp: sendto fail(%d)\n", ret);
        return -1;
    }
    // Add receiving peers to a linked list
    socket = global_socket;
    
    new_recv_peer = (struct rudp_client*)malloc(sizeof(struct rudp_client));
    if(new_recv_peer == NULL) 
    {
        fprintf(stderr, "rudp_send_ack_packet: Memory allocation failed.\n");           
        return -1;
    }
    printf("Send ACK to %s:%d\n", inet_ntoa(to->sin_addr), ntohs(to->sin_port));    
    memset(new_recv_peer, 0x0, sizeof(struct rudp_client));
    
    new_recv_peer->status = SENDING;
    new_recv_peer->seq = seq_num-1;            // Checking for packet drop
    new_recv_peer->rsock_addr = *to;
    
    socket->incoming_peer = new_recv_peer;
    
    return 0;
}
/*===================================================================================
 * Send an ACK Packet and delete recevier peer list
 *===================================================================================*/
int rudp_process_fin_msg(struct rudp_socket_type *rsocket, struct sockaddr_in *to, unsigned int seq_num)
{
    rudp_packet_t rudp_ack;
    struct rudp_socket_type *socket;
    int ret = 0;
    
    memset(&rudp_ack, 0x0, sizeof(rudp_packet_t));
    
    rudp_ack.header.version = RUDP_VERSION;
    rudp_ack.header.type = RUDP_ACK;
    rudp_ack.header.seqno = seq_num;
    
    ret = sendto((int)rsocket->socketfd, (void *)&rudp_ack, sizeof(rudp_packet_t), 0,
                 (struct sockaddr*)to, sizeof(struct sockaddr_in));
    if(ret <= 0)
    {
        fprintf(stderr, "rudp: sendto fail(%d)\n", ret);
        return -1;
    }
    printf("Send FIN ACK to %s:%d\n", inet_ntoa(to->sin_addr), ntohs(to->sin_port));        
    
    socket = (struct rudp_socket_type*) rsocket;     
    if (socket->incoming_peer == NULL)
        fprintf(stderr, "INC PEER AR NULL");
    
    return 0;
    
}

void rudp_add_packet(struct send_packet_structure **head, struct send_packet_structure *packet)
{
    struct send_packet_structure *current, *prev;
    
    current = *head;
    prev = current;
    
    while(current != NULL)
    {
        prev = current;
        current = current->next_buff;
    }
    
    current = packet;
    current->send_flag = 0;
    if(prev != NULL)
    {
        prev->next_buff = current;
    }
    else
    {
        *head = current;
    }
}

void rudp_add_window(struct send_data_window **window_head, struct send_packet_structure *buffer)
{
    struct send_data_window *current, *prev;
    
    current = *window_head;
    prev = current;
    
    while(current != NULL)
    {
        prev = current;
        current = current->next_buff;
    }
    
    current = (struct send_data_window *)malloc(sizeof(struct send_data_window));
    
    current->send_flag = 0;
    current->rudp_packet = &buffer->rudp_packet;
    current->len = buffer->len;
    current->next_buff = NULL;
    if(prev != NULL)
    {
        prev->next_buff = current;
    }
    else
    {
        *window_head = current;
    }
    
}

struct send_packet_structure *prepare_packet(int type, unsigned int seq, void *data, int len) 
{
    struct send_packet_structure *buffer = NULL;
    buffer = (struct send_packet_structure *)malloc(sizeof(struct send_packet_structure));
    memset(buffer, 0x0, sizeof(struct send_packet_structure));
    
    buffer->send_flag = 0;
    buffer->rudp_packet.header.version = RUDP_VERSION;
    buffer->rudp_packet.header.type = type;
    buffer->rudp_packet.header.seqno = seq;
    buffer->transcnt = 0;
    
    memcpy(buffer->rudp_packet.data, data, len);
    buffer->len = len;
    
    
    buffer->next_buff = NULL;
    
    return buffer;     
}

/* 
 * rudp_socket: Create a RUDP socket. 
 * May use a random port by setting port to zero. 
 */

rudp_socket_t rudp_socket(int port) 
{
    int sockfd = -1;
    struct sockaddr_in in;  
    int port_num=100;
    
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd < 0)
    {
        fprintf(stderr, "rudp: socket error : ");
        return NULL;
    }
    port_num = port;
    printf("\n");
    printf("rudp_socket: Socketfd: %d, Port number: %d\n", sockfd, port_num);
    
    bzero(&in, sizeof(in));
    
    in.sin_family = AF_INET;
    in.sin_addr.s_addr = htonl(INADDR_ANY);
    in.sin_port = htons(port_num);
    
    if(bind(sockfd, (struct sockaddr *)&in, sizeof(in)) == -1)
    {
        fprintf(stderr, "rudp: bind error\n");
        return NULL;
    }
    // create rudp_socket
    global_socket = (struct rudp_socket_type*)malloc(sizeof(struct rudp_socket_type));
    global_socket->socketfd = sockfd;
    global_socket->rsock_addr = (struct sockaddr_in) in;
    global_socket->outgoing_peer = NULL;
    global_socket->incoming_peer = NULL;
    global_socket->recvfrom_handler_callback = NULL;
    global_socket->event_handler_callback = NULL;
    
    
    // Registeration a function for receiving socket data from Network
    if(event_fd((int)sockfd, &rudp_receive_data, (void*)global_socket, "rudp_receive_data") < 0)
    {
        printf("[Error] event_fd failed: rudp_receive_data()\n");
        return NULL;
    }
    return (rudp_socket_t*)global_socket;
}

/* 
 *rudp_close: Close socket 
 */ 

int rudp_close(rudp_socket_t rsocket) {
    struct send_packet_structure *packet_buff;   // pointer to packet buffer
    unsigned int seqno;                     // packet sequence number
    struct rudp_socket_type *socket = (struct rudp_socket_type *) rsocket;
    
    socket->outgoing_peer->status = SENDING;
    
    // append FIN to the send buffer
    seqno = socket->outgoing_peer->seq;
    packet_buff = prepare_packet(RUDP_FIN, seqno+1, NULL, 0);
    rudp_add_packet(&socket->outgoing_peer->queue_buff, packet_buff);
    socket->outgoing_peer->seq = seqno+1;
    
    // try to transmit data (if window is available)
    transmit(socket->outgoing_peer, socket->socketfd);
    
    return 0;
}

/* 
 *rudp_recvfrom_handler: Register receive callback function 
 */ 

int rudp_recvfrom_handler(rudp_socket_t rsocket, 
			  int (*handler)(rudp_socket_t, struct sockaddr_in *, 
					 char *, int)) {
    struct rudp_socket_type *socket;
    
    printf("rudp: rudp_recvfrom_handler.\n");
    socket = (struct rudp_socket_type*)rsocket;
    socket->recvfrom_handler_callback = handler;               // rudp_receiver
    return 0;
}

/* 
 *rudp_event_handler: Register event handler callback function 
 */ 
int rudp_event_handler(rudp_socket_t rsocket, 
		       int (*handler)(rudp_socket_t, rudp_event_t, 
				      struct sockaddr_in *)) {
    struct rudp_socket_type *socket;
    
    printf("rudp: rudp_event_handler.\n");
    socket = (struct rudp_socket_type*)rsocket;
    socket->event_handler_callback = handler;
    return 0;
}


/* 
 * rudp_sendto: Send a block of data to the receiver. 
 */

int rudp_sendto(rudp_socket_t rsocket, void* data, int len, struct sockaddr_in* to) 
{
    unsigned int seqno;
    struct send_packet_structure *packet_buff;
    struct rudp_socket_type *socket = (struct rudp_socket_type *) rsocket;
    if(socket->outgoing_peer != NULL)
    {	
        seqno = socket->outgoing_peer->seq;
        packet_buff = prepare_packet(RUDP_DATA, seqno+1, data, len);
        rudp_add_packet(&socket->outgoing_peer->queue_buff, packet_buff);
    
        // increase the sequence number
        socket->outgoing_peer->seq = seqno+1;
    }
    else
    {
        socket->outgoing_peer = (struct rudp_client*)malloc(sizeof(struct rudp_client));
        socket->outgoing_peer->status = INITIAL;
        socket->outgoing_peer->rsock_addr = *to;
        socket->outgoing_peer->seq = 0;
        
        // set up random sequence number
        srand(time(NULL));
        seqno = rand() % 0xFFFFFFFF + 1;
        
        // add the data to the send peer buffer: SYN
        packet_buff = prepare_packet(RUDP_SYN, seqno, NULL, 0); 
        rudp_add_packet(&socket->outgoing_peer->queue_buff, packet_buff);
        
        // add the data to the send peer buffer: DATA
        packet_buff = prepare_packet(RUDP_DATA, seqno+1, data, len); 
        rudp_add_packet(&socket->outgoing_peer->queue_buff, packet_buff);
        socket->outgoing_peer->seq = seqno+1;
        
        // start transmission to the peer
        printf("rudp: Debug: Start transmission.\n");
        transmit(socket->outgoing_peer, socket->socketfd);
    }
    return 0;
}

int rudp_receive_data(int fd, void *arg)
{
    struct rudp_socket_type *rsocket = (struct rudp_socket_type*)arg;
    struct sockaddr_in dest_addr;
    rudp_packet_t rudp_data;
    int addr_size;
    int bytes=0;
    
    memset(&rudp_data, 0x0, sizeof(rudp_packet_t));
    
    addr_size = sizeof(dest_addr);
    bytes = recvfrom((int)fd, (void*)&rudp_data, sizeof(rudp_data), 
                     0, (struct sockaddr*)&dest_addr, (socklen_t*)&addr_size);
    if(bytes <= 0)
    {
        printf("[Error]: recvfrom failed(fd=%d).\n", fd);
        return -1;
    }
    bytes -= sizeof(struct rudp_hdr);               // Only Data size
    
    rsocket->rsock_addr = dest_addr;
    
    rudp_process_received_packet((void*)&rudp_data, rsocket, bytes);
    return 0;
}

void rudp_process_received_packet(void *buf, struct rudp_socket_type *rsocket, int len)
{
    rudp_packet_t *rudp_data = NULL;
    struct sockaddr_in from;
    unsigned int seq_num;
    int type;
    
    rudp_data = (rudp_packet_t*)buf;
    from = rsocket->rsock_addr;
    type = rudp_data->header.type;
    seq_num = rudp_data->header.seqno;
    
    switch(type)
    {
        case RUDP_DATA:
            printf("rudp: RUDP_DATA (seq: %0x)\n", seq_num);
            // Send RUDP_ACK                
            rudp_send_data_ack(rsocket, &from, seq_num, rudp_data->data, len);
            break;
        case RUDP_ACK:
            printf("rudp: RUDP_ACK (seq: %0x) from %s:%d\n", seq_num, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
            // Send RUDP_DATA
            rudp_send_data(rsocket, &from, seq_num);
            break;
        case RUDP_SYN:
            printf("rudp: RUDP_SYN (seq: %0x) from %s:%d\n", seq_num, inet_ntoa(from.sin_addr), ntohs(from.sin_port));              
            // Send RUDP_ACK
            rudp_send_ack_packet(rsocket, &from, seq_num+1);
            break;
        case RUDP_FIN:
            printf("rudp: RUDP_FIN (seq: %0x) from %s:%d\n", seq_num, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
            rudp_fin_received = TRUE;
            // Send RUDP_ACK
            rudp_process_fin_msg(rsocket, &from, seq_num+1);
            break;
        default:
            break;
    }
    
}

int transmit(struct rudp_client *send_peer, int socketfd)
{
    int wcnt;                               // counter of window occupancy
    struct send_data_window *window;        // pointer to the send window
    struct send_packet_structure *queue;    // pointer to the send queue
    unsigned int cnt, length, seq, dumcnt;  // dummy variables
    int socket;                             // socket file descriptor
    struct sockaddr_in rsock_addr;          // socket address
    struct timeval timer, t0, t1;           // timer variables
    
    // initialize timer variables
    timer.tv_sec = timer.tv_usec = 0;
    t0.tv_sec = t0.tv_usec = 0;
    t1.tv_sec = t1.tv_usec = 0;
    
    // check amount of packets in the window
    queue = send_peer->queue_buff;
    window = send_peer->window;
    wcnt = 0;
    dumcnt = 0;
    while(window != NULL)
    {
        wcnt++;
        window = window->next_buff;
        queue = queue->next_buff;       // shift the queue buffer pointer because the data is in window already
    }
    
    // if less than RUDP_WINDOW and there still data in queue, copy data from queue buffer to fill in the window
    if((wcnt < RUDP_WINDOW) && (queue != NULL))
    {
        printf("rudp: Window size: %d.\n", (RUDP_WINDOW-wcnt));
        for(cnt=0; cnt<(RUDP_WINDOW-wcnt); cnt++)
        {
            rudp_add_window(&send_peer->window, queue);
            queue = queue->next_buff;
            dumcnt++;
            
            if(queue == NULL)       // no more data
            {
                break;
            }
        }
        printf("rudp: Added %d packets to the window.\n", dumcnt);
    }
    
    // transmit all untransmitted data in window
    window = send_peer->window;
    socket = socketfd;
    rsock_addr = send_peer->rsock_addr;
    dumcnt = 0;
    
    for(cnt=0; cnt<RUDP_WINDOW && window != NULL; cnt++)
    {
        // if INITIAL, send SYN
        if(send_peer->status == INITIAL && window->send_flag == 0)
        {
            seq = window->rudp_packet->header.seqno;
            length = sizeof(window->rudp_packet->header) + window->len;
            
            if(dropped) 
            {
               
                printf("rudp: Dropped packet to (%s:%d) via socket (%d) seq (%d).\n", 
                inet_ntoa(rsock_addr.sin_addr), ntohs(rsock_addr.sin_port), socket, seq);
                dropped = FALSE;
                
            }
            else{
                
                dropped = TRUE;
            if(sendto(socket, window->rudp_packet, length, 0, (struct sockaddr *)&rsock_addr, sizeof(rsock_addr)) <= 0)
            {
                perror("Error in send_to: ");
                return -1;
            }
            printf("rudp: Packet sent to (%s:%d) via socket (%d) seq (%d).\n", 
            inet_ntoa(rsock_addr.sin_addr), ntohs(rsock_addr.sin_port), socket, seq);
            }
            
            window->send_flag = 1;  // set to status to sent
            dumcnt++;               // increment packet counter
            
            // check whether the data is SYN
            if(window->rudp_packet->header.type == RUDP_SYN)
            {
                // if so, set status to FINISHED
                send_peer->status = SYN_SENT;
                printf("rudp: SYN sent to %s:%d\n", inet_ntoa(rsock_addr.sin_addr), ntohs(rsock_addr.sin_port));
            }
            
            // Start the timeout callback with event_timeout
            timer.tv_sec = RUDP_TIMEOUT/1000;               // convert to second
            timer.tv_usec = (RUDP_TIMEOUT%1000) * 1000;     // convert to micro
            gettimeofday(&t0, NULL);                        // current time of the day
            timeradd(&t0, &timer, &t1);  //add the timeout time with thecurrent time of the day
            
            // register timeout
            if(event_timeout(t1, &retransmit, window->rudp_packet, "timer_callback") == -1)
            {
                perror("rudp: Error registering event_timeout\n");
                return -1;
            }
            break;
        }
        
        // untransmitted window
        else if(send_peer->status == SENDING && window->send_flag == 0)
        {
            seq = window->rudp_packet->header.seqno;
            length = sizeof(window->rudp_packet->header) + window->len;
            
            if(dropped) 
            {
                
                printf("rudp: Dropped packet to (%s:%d) via socket (%d) seq (%0x).\n", 
                       inet_ntoa(rsock_addr.sin_addr), ntohs(rsock_addr.sin_port), socket, seq);
                dropped = FALSE;
                
            }
            else{
                
                dropped = TRUE;
        
            if(sendto(socket, window->rudp_packet, length, 0, (struct sockaddr *)&rsock_addr, sizeof(rsock_addr)) <= 0)
            {
                perror("Error in send_to: ");
                return -1;
            }
            printf("rudp: Packet sent to (%s:%d) via socket (%d) seq (%0x).\n", 
            inet_ntoa(rsock_addr.sin_addr), ntohs(rsock_addr.sin_port), socket, seq);
            }
            window->send_flag = 1;  // set to status to sent
            dumcnt++;               // increment packet counter
            
            // check whether the data is FIN
            if(window->rudp_packet->header.type == RUDP_FIN)
            {
                // if so, set status to FINISHED
                send_peer->status = FINISHED;
                printf("rudp: FIN sent to %s:%d\n", inet_ntoa(rsock_addr.sin_addr), ntohs(rsock_addr.sin_port));
            }
            
            // Start the timeout callback with event_timeout
            timer.tv_sec = RUDP_TIMEOUT/1000;               // convert to second
            timer.tv_usec = (RUDP_TIMEOUT%1000) * 1000;     // convert to micro
            gettimeofday(&t0, NULL);                        // current time of the day
            timeradd(&t0, &timer, &t1);  //add the timeout time with thecurrent time of the day
            
            // register timeout
            if(event_timeout(t1, &retransmit, window->rudp_packet, "timer_callback") == -1)
            {
                perror("rudp: Error registering event_timeout\n");
                return -1;
            }
        }
        
        window = window->next_buff;
        if(window == NULL)
        {
            //printf("rudp: Number of packet sent: %d.\n", dumcnt);
            break;
        }
    }
    
    return 0;
}

int retransmit(int fd, void *arg)
{
    struct send_packet_structure *packet_buff;   // pointer to packet buffer
    rudp_packet_t *rudp_packet;             // pointer to RUDP packet
    int sockfd;                             // socket file descriptor
    struct sockaddr_in rsock_addr;          // the address of the destination
    int length;                             // transmission data length
    struct timeval timer, t0, t1;           // timer variables
    
    struct rudp_client *send_peer = global_socket->outgoing_peer;
    rudp_packet = (rudp_packet_t *)arg;
    packet_buff = send_peer->queue_buff;
    rsock_addr = send_peer->rsock_addr;
    sockfd = global_socket->socketfd;
    
    // initialize timer variables
    timer.tv_sec = timer.tv_usec = 0;
    t0.tv_sec = t0.tv_usec = 0;
    t1.tv_sec = t1.tv_usec = 0;
    
    if(packet_buff->transcnt < RUDP_MAXRETRANS)
    {
        length = packet_buff->len + sizeof(rudp_packet->header);
        packet_buff->transcnt++;
        
        printf("rudp: retransmit packet seq=%0x to %s:%d\n", 
               rudp_packet->header.seqno, inet_ntoa(rsock_addr.sin_addr), ntohs(rsock_addr.sin_port));
        
        if(sendto(sockfd, rudp_packet, length, 0, (struct sockaddr *)&rsock_addr, sizeof(rsock_addr)) <= 0)
        {
            perror("Error in send_to: ");
            return -1;
        }
        
        // register timeout again
        timer.tv_sec = RUDP_TIMEOUT/1000;               // convert to second
        timer.tv_usec = (RUDP_TIMEOUT%1000) * 1000;     // convert to micro
        gettimeofday(&t0, NULL);                        // current time of the day
        timeradd(&t0, &timer, &t1);  //add the timeout time with thecurrent time of the day
        
        if(event_timeout(t1, &retransmit, &packet_buff->rudp_packet, "timer_callback") == -1)
        {
            perror("rudp: Error registering event_timeout\n");
            return -1;
        }
        return 0;
    }
    else
    {
        printf("rudp: Signaling RUDP_EVENT_TIMEOUT to application\n");
        global_socket->event_handler_callback((rudp_socket_t)global_socket, RUDP_EVENT_TIMEOUT, NULL);
        printf("rudp: Retransmission count exceeded the limit %d times.\n", RUDP_MAXRETRANS);
        return -1;
    }
    

    
    
}








