/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"

typedef struct {
  long  lastTimeSent; 
  uint32_t numberXmit;  /*number of retransmit*/
  ctcp_segment_t segment;
} packet_t;

/* Add function.*/
void ctcpSendWindow(ctcp_state_t *state);
void ctcpSendSegment(ctcp_state_t *state, packet_t *packet);
void ctcpSendAck(ctcp_state_t *state);
void ctcpRemoveAckedSegment(ctcp_state_t *state);



/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */

  conn_t *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */
  linked_list_t *sendList;  /* Linked list of segments sent to this connection.
                               It may be useful to have multiple linked lists
                               for unacknowledged segments, segments that
                               haven't been sent, etc. Lab 1 uses the
                               stop-and-wait protocol and therefore does not
                               necessarily need a linked list. You may remove
                               this if this is the case for you */
  linked_list_t *recvList;
  /* FIXME: Add other needed fields. */
  ctcp_config_t *config;
  uint32_t lastSeqnoRead;  /*last bytes read from stdin*/
  uint32_t lastSeqnoAccept;  /*last bytes output to stdout*/
  uint32_t lastAcknoReceive;  /*last ACK received*/

  bool readEOF;             /*Read EOF from stdin*/
  bool recvFIN;             /*FIN segment received*/
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */


ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
  /* Connection could not be established. */
  if (conn == NULL) {
    return NULL;
  }

  /* Established a connection. Create a new state and update the linked list
     of connection states. */
  ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
  state->next = state_list;
  state->prev = &state_list;
  if (state_list)
    state_list->prev = &state->next;
  state_list = state;

  /* Set fields. */
  state->conn = conn;
  /* FIXME: Do any other initialization here. */
  // state->config->recv_window = cfg->recv_window;
  // state->config->send_window = cfg->send_window;
  // state->config->timer  = cfg->timer;
  // state->config->rt_timeout = cfg->rt_timeout;

  state->config = cfg;

  state->lastSeqnoRead = 0;
  state->lastSeqnoAccept = 0;
  state->lastAcknoReceive = 0;

  state->readEOF = false;
  state->recvFIN = false;
  
  state->sendList = ll_create();     /*Create linked list sender 
                                        && receiver*/
  state->recvList = ll_create();
  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  /* FIXME: Do any other cleanup here. */

  free(state);
  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  /* FIXME */

  int buf[MAX_SEG_DATA_SIZE];
  // void *buff = calloc()
  int bytesRead;
  packet_t *packet;

  /*State EOF from stdin*/
  if (state->readEOF)
  {
    return;
  }

  /*Read data, add data segment to send linked list*/
  while ((bytesRead = conn_input(state->conn, buf, MAX_SEG_DATA_SIZE)) > 0)
  {
    packet = (packet_t*)calloc(1, sizeof(packet_t) + bytesRead);
    assert(packet != NULL);
    packet->segment.len = htons((uint16_t)sizeof(ctcp_segment_t) + bytesRead);
    packet->segment.seqno = htonl(state->lastSeqnoRead + 1);
    memcpy(packet->segment.data, buf, bytesRead);
    state->lastSeqnoRead += bytesRead;
    ll_add(state->sendList, packet);
  }

  /*Read EOF or error, add FIN segment to send linked list*/
  if (bytesRead == -1)
  {
    state->readEOF = true;     /*Update state*/
    packet = (packet_t *)calloc(1, sizeof(packet_t));
    assert(packet != NULL);
    packet->segment.len = htons((uint16_t)sizeof(ctcp_segment_t));
    packet->segment.seqno = htonl(state->lastSeqnoRead + 1);
    packet->segment.flags |= TH_FIN;
    ll_add(state->sendList, packet);
  }
  /*Send window to received side. */
  ctcpSendWindow(state);
}

void ctcpSendWindow(ctcp_state_t *state)
{
  packet_t *packet;
  ll_node_t *node;
  uint32_t lastSeqnoSegment;
  uint32_t lastSeqnoWindow;
  long msSinceLastSent;
  uint16_t datalen;
  unsigned int ll_len , i;

  if (state == NULL)
  {
    return;
  }
  ll_len = ll_length(state->sendList);   /*Length of linked list*/
  if (ll_len == 0)
  {
    return;
  }
  /*Handle window size > 0*/
  for (i = 0;i < ll_len; ++i)
  { 
    /*Case 1 node*/
    if (i == 0)  
    {
      node = ll_front(state->sendList);
    }
    else   /*More 1 node*/
    {
      node = node->next;
    }
    packet = (packet_t*)node->object;
    datalen = ntohs(packet->segment.len) - sizeof(ctcp_segment_t);
    lastSeqnoSegment = ntohl(packet->segment.seqno) + datalen - 1;
    lastSeqnoWindow = state->lastAcknoReceive + state->config->send_window - 1;
    if (state->lastAcknoReceive == 0)
    {
      ++lastSeqnoWindow;
    }
    if (lastSeqnoSegment > lastSeqnoWindow)
    { 
      /*maintain the segment allowable in sliding window*/
      return;
    }

    /*Check segment if sent or not*/
    if (packet->numberXmit == 0)
    {
      ctcpSendSegment(state, packet);
    }
    else
    {
      /*the segment was sent so check if reached timeout and re-send it*/
      msSinceLastSent = current_time() - packet->lastTimeSent;
      if(msSinceLastSent > state->config->rt_timeout)
      {
        ctcpSendSegment(state, packet);
      }
    }
  }
}

void ctcpSendSegment(ctcp_state_t *state, packet_t *packet)
{
  long time_sent;
  uint16_t segment_cksum;
  int bytes_sent;

  /*Check segment retransmit*/
  if (packet->numberXmit >= MAX_NUM_XMITS)
  {
    ctcp_destroy(state);
    return;
  }
  
  /*Set up packet to send*/
  packet->segment.ackno = htonl(state->lastSeqnoAccept + 1);
  packet->segment.flags |= TH_ACK;
  packet->segment.window = htons(state->config->recv_window);
  packet->segment.cksum = 0;
  segment_cksum = cksum(&packet->segment, ntohs(packet->segment.len));
  packet->segment.cksum = segment_cksum;

  /*Send packet to receiver*/
  bytes_sent = conn_send(state->conn, &packet->segment, ntohs(packet->segment.len));

  time_sent = current_time();
  packet->numberXmit++;

  /*Corrupt when send packet*/
  if (bytes_sent < ntohs(packet->segment.len))
  {
    return;
  }
  if(bytes_sent == -1)
  {
    ctcp_destroy(state);
    return;
  }

  packet->lastTimeSent = time_sent;
}


void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /* FIXME */
  uint16_t send_cksum, recv_cksum, datalen;
  uint32_t lastSeqnoSegment, min_seqno_allow, max_seqno_allow;
  ll_node_t *node;
  ctcp_segment_t *segment_ptr;
  unsigned int ll_len, i;

  /* Truncated segment */
  if (len < ntohs(segment->len))
  {
    free(segment);
    return;
  }
  
  /*Handle corrupt segment*/
  send_cksum = segment->cksum;
  segment->cksum = 0;
  recv_cksum = cksum(segment, ntohs(segment->len));
  segment->cksum = send_cksum;
  if (send_cksum != recv_cksum)
  {
    free(segment);
    return;
  }

  /*Check if the segment is allowable in received window */
  datalen = ntohs(segment->len) - sizeof(ctcp_segment_t);
  if(datalen)
  {
    lastSeqnoSegment = ntohl(segment->seqno) + datalen - 1;
    min_seqno_allow = state->lastSeqnoAccept + 1;
    max_seqno_allow = state->lastSeqnoAccept + state->config->recv_window;
    
    if((lastSeqnoSegment < min_seqno_allow) ||
        (lastSeqnoSegment > max_seqno_allow))
    {
      free(segment);
      ctcpSendAck(state);
      return;
    }
  }
  if (segment->flags & TH_ACK)
  {
    /*It an ACK segment of FIN segment so update the last acknowledged number */
    state->lastAcknoReceive = ntohl(segment->ackno);
  }
  
  /*Receive data segment or FIN segment*/
  if ((datalen > 0) || (segment->flags & TH_FIN))
  {
    /*Handle receive linked list*/
    ll_len = ll_length(state->recvList);
    /*List is empty*/
    if (ll_len == 0)      
    {
      ll_add(state->recvList, segment);
    }
    /*List has 1 node*/
    else if (ll_len == 1) 
    {
      node = ll_front(state->recvList);
      segment_ptr = (ctcp_segment_t *)node->object;

      /*Segment receives is retransmit segment exist in receive list*/
      if (ntohl(segment->seqno) == ntohl(segment_ptr->seqno))
      {
        free(segment);
      }
      /*Add segment to right position in receive list*/
      else if (ntohl(segment->seqno) > ntohl(segment_ptr->seqno))
      {
        ll_add(state->recvList, segment);
      }
      else 
      {
        ll_add_front(state->recvList, segment);
      }
    }
    /*List more than 1 node*/
    else
    {
      ll_node_t *first_node;
      ll_node_t *last_node;
      ctcp_segment_t *first_segment;
      ctcp_segment_t *last_segment;
      
      /*Fist node, last node and respectively first segment, last segment
        in receive list*/
      first_node = ll_front(state->recvList);
      last_node = ll_back(state->recvList);
      first_segment = (ctcp_segment_t*)first_node->object;
      last_segment = (ctcp_segment_t*)last_node->object;

      /*Add segment to right position*/
      if (ntohl(segment->seqno) < ntohl(first_segment->seqno)) /*Add to first list*/
      { 
        ll_add_front(state->recvList, segment);
      }
      else if (ntohl(segment->seqno) > ntohl(last_segment->seqno)) /*Add to last list*/
      {
        ll_add(state->recvList, segment);
      }
      else
      {
        ll_node_t *curr_node;
        ll_node_t *next_node;
        ctcp_segment_t *curr_segment;
        ctcp_segment_t *next_segment;

        /*Fit position for segment*/
        for (i = 0;i < (ll_len - 1); ++i)
        { 
          /*Get curr segment and next segment*/
          if (i == 0)
	        {
            curr_node = ll_front(state->recvList);
          }
          else
          {
            curr_node = curr_node->next;
          }
          next_node = curr_node->next;
          curr_segment = (ctcp_segment_t*)curr_node->object;
          next_segment = (ctcp_segment_t*)next_node->object;

          if ((ntohl(segment->seqno) == ntohl(curr_segment->seqno)) ||
              (ntohl(segment->seqno) == ntohl(next_segment->seqno)))
          {
            /*Segment is retransmit of one segment in receive list*/
            free(segment);
          }
          else
 	        {  
            /*Add to right position n receive list*/
            if ((ntohl(segment->seqno) > ntohl(curr_segment->seqno)) &&
                (ntohl(segment->seqno) < ntohl(next_segment->seqno)))
            {
              ll_add_after(state->recvList,curr_node, segment);
              break;
            }
          }
	      }
      }
    }
  }
  else
  {
    /*Receive ACK segment*/
    free(segment);
  }
  /*Output correct order from receive list*/
  ctcp_output(state);

  /*Remove ACK segment from receive list*/
  ctcpRemoveAckedSegment(state);
}



void ctcp_output(ctcp_state_t *state) {
  /* FIXME */
  ll_node_t *node;
  ctcp_segment_t *segment;
  size_t bufspace;
  uint16_t datalen;
  int cnt_output = 0;
  int bytes_output;
  
  if (state == NULL)
  {
    return;
  }
  
  while (ll_length(state->recvList) != 0)
  {  
    /*Handle from first element in recieve list*/
    node = ll_front(state->recvList);
    segment = (ctcp_segment_t*)node->object;
    datalen = ntohs(segment->len) - sizeof(ctcp_segment_t);
    if(datalen)
    { 
      /*Segment is not in correct order*/
      if (ntohl(segment->seqno) != (state->lastSeqnoAccept + 1))
      {
        return;
      }

      /*Check bufspace is allowed*/
      bufspace = conn_bufspace(state->conn);
      if (datalen > bufspace)
      {
        return;
      }
      bytes_output = conn_output(state->conn, segment->data, datalen);
      if (bytes_output == -1)
      {
        ctcp_destroy(state);
        return;
      }

      cnt_output++;
    }
    
    /*Update last_seq_ack in state if ouput data segment*/
    if(datalen)
    {
      state->lastSeqnoAccept += datalen;
    }

    /*If segment is FIN segment*/
    if((!state->recvFIN) && (segment->flags & TH_FIN))
    { 
      /*Update state*/
      state->recvFIN = true;
      state->lastSeqnoAccept++;
      /*Output FIN segment*/
      conn_output(state->conn, segment->data, 0);

      cnt_output++;
    }
    free(segment);
    
    /*Remove packet from receive list*/
    ll_remove(state->recvList, node); 
  }
  if(cnt_output)
  {
    /*Handle ACK for segment is outputed*/
    ctcpSendAck(state);
  }
}
void ctcpSendAck(ctcp_state_t *state)
{
  /*Setup ACK segment*/
  ctcp_segment_t *segment;
  segment = (ctcp_segment_t*)calloc(1, sizeof(ctcp_segment_t));
  segment->seqno = htonl(0);
  segment->ackno = htonl(state->lastSeqnoAccept + 1);
  segment->len = htons(sizeof(ctcp_segment_t));
  segment->flags |= TH_ACK;
  segment->window = htons(state->config->recv_window);
  segment->cksum = 0;
  segment->cksum = cksum(segment, ntohs(segment->len));

  /*Send ACK segment*/
  conn_send(state->conn, segment, ntohs(segment->len));
}

void ctcpRemoveAckedSegment(ctcp_state_t *state)
{
  uint16_t datalen;
  uint32_t lastSeqnoSegment;
  ll_node_t *node;
  packet_t *packet;

  while (ll_length(state->sendList) != 0)
  {
    node = ll_front(state->sendList);
    packet = (packet_t*)node->object;
    datalen = ntohs(packet->segment.len) - sizeof(ctcp_segment_t);
    lastSeqnoSegment = ntohl(packet->segment.seqno) + datalen - 1;

    /*Remove packet from receive list*/
    if (lastSeqnoSegment < state->lastAcknoReceive) 
    {
      free(packet);
      ll_remove(state->sendList, node);
    }
    else
    {
      return;
    }
  }
}
void ctcp_timer() {
  /* FIXME */
  ctcp_state_t *curr_state;
  for (curr_state = state_list; curr_state != NULL; curr_state = curr_state->next)
  { 
    ctcp_output(curr_state);
    ctcpSendWindow(curr_state);

    if((curr_state->readEOF) && (curr_state->recvFIN) &&
        (ll_length(curr_state->sendList) == 0) && (ll_length(curr_state->recvList) == 0))
    {
      ctcp_destroy(curr_state);
    }
  }  
}