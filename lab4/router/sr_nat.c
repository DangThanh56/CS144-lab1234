
#include <signal.h>
#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sr_nat.h"
#include "sr_protocol.h"
#include "sr_router.h"
#include "sr_utils.h"

static const char internalInterfaceName[] = "eth1";

/*Get internal interface*/
static sr_if_t* getInternalInterface(sr_instance_t *sr)
{
   return sr_get_interface(sr, internalInterfaceName);
}


/* Declare all function here.*/
static void sr_nat_destroy_mapping(sr_nat_t* nat, sr_nat_mapping_t* natMapping);
struct sr_nat_mapping *sr_nat_lookup_external(struct sr_nat *nat,
                     uint16_t aux_ext, sr_nat_mapping_type type );
static sr_nat_mapping_t *natLookupExternal(sr_nat_t *nat, uint16_t aux_ext,
                                       sr_nat_mapping_type type);                     
struct sr_nat_mapping *sr_nat_lookup_internal(struct sr_nat *nat, uint32_t ip_int,
                      uint16_t aux_int, sr_nat_mapping_type type );
static sr_nat_mapping_t *natLookupInternal(sr_nat_t *nat, uint32_t ip_int, uint16_t aux_int,
                                       sr_nat_mapping_type type);
struct sr_nat_mapping *sr_nat_insert_mapping(struct sr_nat *nat, uint32_t ip_int, uint16_t aux_int,
                                       sr_nat_mapping_type type);
static sr_nat_mapping_t *natCreateMapping(sr_nat_t *nat, uint32_t ip_int, uint16_t aux_int,
                                       sr_nat_mapping_type type);
static uint16_t natNextMappingNumberAux(sr_nat_t* nat, sr_nat_mapping_type type);
void natHandleRecievedIpPacket(struct sr_instance* sr, sr_ip_hdr_t* ipPacket, unsigned int length,
                                       sr_if_t *receivedInterface);
static void natHandleTcpPacket(sr_instance_t* sr, sr_ip_hdr_t* ipPacket, unsigned int length,
                                       sr_if_t *receivedInterface);                                                                              
static sr_nat_connection_t *natFindConnection(sr_nat_mapping_t *natEntry, uint32_t ip_ext, 
                                                uint16_t port_ext);
static void natHandleReceivedOutboundIpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet,
   unsigned int length, struct sr_if* receivedInterface, sr_nat_mapping_t *natMapping);
static void natHandleReceivedInboundIpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet, 
   unsigned int length, struct sr_if* receivedInterface, sr_nat_mapping_t *natMapping);
static void natHandleIcmpPacket(sr_instance_t* sr, sr_ip_hdr_t* ipPacket, unsigned int length,
                                                            sr_if_t *receivedInterface);
void natNotdonePacketMapping(struct sr_instance* sr, sr_ip_hdr_t* ipDatagram, unsigned int length, 
                                                            sr_if_t *receivedInterface);
static void sr_nat_destroy_connection(sr_nat_mapping_t* natMapping, sr_nat_connection_t* connection);
static void natRecalculateTcpChecksum(sr_ip_hdr_t *tcpPacket, unsigned int length);

static uint16_t getIpHeaderLength(sr_ip_hdr_t *pkt)
{
  return (pkt->ip_hl) * 4;
}

static sr_icmp_hdr_t* getIcmpHeaderFromIpHeader(sr_ip_hdr_t * packet)
{
  return (sr_icmp_hdr_t*) (((uint8_t*) packet) + getIpHeaderLength(packet));
}

static sr_tcp_hdr_t * getTcpHeaderFromIpHeader(sr_ip_hdr_t * packet)
{
  return (sr_tcp_hdr_t*) (((uint8_t*) packet) + getIpHeaderLength(packet));
}

int sr_nat_init(struct sr_nat *nat) { /* Initializes the nat */

  assert(nat);

  /* Acquire mutex lock */
  pthread_mutexattr_init(&(nat->attr));
  pthread_mutexattr_settype(&(nat->attr), PTHREAD_MUTEX_RECURSIVE);
  int success = pthread_mutex_init(&(nat->lock), &(nat->attr));

  /* Initialize timeout thread */

  pthread_attr_init(&(nat->thread_attr));
  pthread_attr_setdetachstate(&(nat->thread_attr), PTHREAD_CREATE_JOINABLE);
  pthread_attr_setscope(&(nat->thread_attr), PTHREAD_SCOPE_SYSTEM);
  pthread_attr_setscope(&(nat->thread_attr), PTHREAD_SCOPE_SYSTEM);
  pthread_create(&(nat->thread), &(nat->thread_attr), sr_nat_timeout, nat);

  /* CAREFUL MODIFYING CODE ABOVE THIS LINE! */

  nat->mappings = NULL;
  /* Initialize any variables here */
  nat->nextIcmpIdentNumber = START_PORT_NUMBER;
  nat->nextTcpPortNumber = START_PORT_NUMBER;

  return success;
}


int sr_nat_destroy(struct sr_nat *nat) {  /* Destroys the nat (free memory) */

  pthread_mutex_lock(&(nat->lock));

  /* free nat memory here */
  while (nat->mappings)
  {
    sr_nat_destroy_mapping(nat, nat->mappings);
  }
  pthread_kill(nat->thread, SIGKILL);
  return pthread_mutex_destroy(&(nat->lock)) &&
    pthread_mutexattr_destroy(&(nat->attr));

}

/* Removes a mapping from the linked list.*/
static void sr_nat_destroy_mapping(sr_nat_t* nat, sr_nat_mapping_t* natMapping)
{
   if (natMapping)
   {
      sr_nat_mapping_t *req, *prev = NULL, *next = NULL;
      for (req = nat->mappings; req != NULL; req = req->next)
      {
         if (req == natMapping)
         {
            if (prev)
            {
               next = req->next;
               prev->next = next;
            }
            else
            {
               next = req->next;
               nat->mappings = next;
            }
            
            break;
         }
         prev = req;
      }
      
      while (natMapping->conns != NULL)
      {
         sr_nat_connection_t *curr = natMapping->conns;
         natMapping->conns = curr->next;

         free(curr);
      }
      free(natMapping);
   }
}

void *sr_nat_timeout(void *nat_ptr) {  /* Periodic Timout handling */
  struct sr_nat *nat = (struct sr_nat *)nat_ptr;
  while (1) {
    sleep(1.0);
    pthread_mutex_lock(&(nat->lock));

    time_t curtime = time(NULL);

    /* handle periodic tasks here */
      sr_nat_mapping_t *mappingWalker = nat->mappings;
      
      while(mappingWalker)
      {
        if (mappingWalker->type == nat_mapping_icmp)
        {
          if (difftime(curtime, mappingWalker->last_updated) > nat->icmpTimeout)
          {
              mappingWalker = mappingWalker->next;
          }
          else
          {
              mappingWalker = mappingWalker->next;
          }
        }
        else if (mappingWalker->type == nat_mapping_tcp)
        {
          sr_nat_connection_t *connectionWalker = mappingWalker->conns;
          while (connectionWalker)
          {
              if ((connectionWalker->connectionState == nat_conn_connected)
                && (difftime(curtime, connectionWalker->lastAccessed)
                    > nat->tcpEstablishedTimeout))
              {
                sr_nat_destroy_connection(mappingWalker, connectionWalker);
                connectionWalker = connectionWalker->next;
              }
              else if (((connectionWalker->connectionState == nat_conn_outbound_syn)
                || (connectionWalker->connectionState == nat_conn_time_wait))
                && (difftime(curtime, connectionWalker->lastAccessed)
                    > nat->tcpTransitoryTimeout))
              {
                sr_nat_destroy_connection(mappingWalker, connectionWalker);
                connectionWalker = connectionWalker->next;
              }
              else if ((connectionWalker->connectionState == nat_conn_inbound_syn_pending)
                && (difftime(curtime, connectionWalker->lastAccessed)
                    > nat->tcpTransitoryTimeout))
              {
                if (connectionWalker->queuedInboundSyn)
                {
                    IpSendTypeThreeIcmpPacket(nat->routerState,
                      icmp_code_destination_port_unreachable,
                      connectionWalker->queuedInboundSyn);
                }
                sr_nat_destroy_connection(mappingWalker, connectionWalker);
                connectionWalker = connectionWalker->next;
              }
              else
              {
                connectionWalker = connectionWalker->next;
              }
          }
          
          if (mappingWalker->conns == NULL)
          {
              sr_nat_destroy_mapping(nat, mappingWalker);
              mappingWalker = mappingWalker->next;
          }
          else
          {
              mappingWalker = mappingWalker->next;
          }
        }
        else
        {
          mappingWalker = mappingWalker->next;
        }
      }
    pthread_mutex_unlock(&(nat->lock));
  }
  return NULL;
}

/* Get the mapping associated with given external port.
   You must free the returned structure if it is not NULL. */
struct sr_nat_mapping *sr_nat_lookup_external(struct sr_nat *nat,
    uint16_t aux_ext, sr_nat_mapping_type type ) {

  pthread_mutex_lock(&(nat->lock));

  /* handle lookup here, malloc and assign to copy */
  struct sr_nat_mapping *copy = NULL;
  sr_nat_mapping_t *lookupExternalResult = natLookupExternal(nat, aux_ext, type); 

  if (lookupExternalResult != NULL)
  {
    lookupExternalResult->last_updated = time(NULL);
    copy = malloc(sizeof(sr_nat_mapping_t));
    memcpy(copy, lookupExternalResult, sizeof(sr_nat_mapping_t));
  }
  pthread_mutex_unlock(&(nat->lock));
  return copy;
}

/*Performs a NAT external lookup returning a shared pointer into the NAT mapping list.*/
static sr_nat_mapping_t *natLookupExternal(sr_nat_t *nat, uint16_t aux_ext,
   sr_nat_mapping_type type)
{
   sr_nat_mapping_t *mappingWalker;

   for (mappingWalker = nat->mappings; mappingWalker != NULL ; mappingWalker =
      mappingWalker->next)
   {
      if ((mappingWalker->type == type) && (mappingWalker->aux_ext == aux_ext))
      {
         return mappingWalker;
      }
   }
   return NULL;
}

/* Get the mapping associated with given internal (ip, port) pair.
   You must free the returned structure if it is not NULL. */
struct sr_nat_mapping *sr_nat_lookup_internal(struct sr_nat *nat,
  uint32_t ip_int, uint16_t aux_int, sr_nat_mapping_type type ) {

  pthread_mutex_lock(&(nat->lock));

  /* handle lookup here, malloc and assign to copy. */
  struct sr_nat_mapping *copy = NULL;
  sr_nat_mapping_t * lookupInternalResult = natLookupInternal(nat, ip_int, aux_int, type);
    
  if (lookupInternalResult != NULL)
  {
   lookupInternalResult->last_updated = time(NULL);
   copy = malloc(sizeof(sr_nat_mapping_t));
   memcpy(copy, lookupInternalResult, sizeof(sr_nat_mapping_t));
  }

  pthread_mutex_unlock(&(nat->lock));
  return copy;
}

/* Performs a NAT internal lookup returning a shared pointer into the NAT mapping list */
static sr_nat_mapping_t *natLookupInternal(sr_nat_t *nat, uint32_t ip_int, uint16_t aux_int,
                                                                  sr_nat_mapping_type type)
{
  sr_nat_mapping_t *mappingWalker;
    
  for (mappingWalker = nat->mappings; mappingWalker != NULL; mappingWalker = mappingWalker->next)
  {
    if ((mappingWalker->type == type) && (mappingWalker->ip_int == ip_int)
        && (mappingWalker->aux_int == aux_int))
    {
        return mappingWalker;
    }
  }
  return NULL;
}

/* Insert a new mapping into the nat's mapping table.
   Actually returns a copy to the new mapping, for thread safety.
 */
struct sr_nat_mapping *sr_nat_insert_mapping(struct sr_nat *nat,
  uint32_t ip_int, uint16_t aux_int, sr_nat_mapping_type type ) {

  pthread_mutex_lock(&(nat->lock));

  /* handle insert here, create a mapping, and then return a copy of it */
  struct sr_nat_mapping *mapping = NULL;
  struct sr_nat_mapping *copy = malloc(sizeof(sr_nat_mapping_t));
  
  memcpy(copy, mapping, sizeof(sr_nat_mapping_t));

  pthread_mutex_unlock(&(nat->lock));
  return mapping;
}

/* creates a NAT mapping in the NAT structure.*/
static sr_nat_mapping_t *natCreateMapping(sr_nat_t *nat, uint32_t ip_int, uint16_t aux_int,
   sr_nat_mapping_type type)
{
   struct sr_nat_mapping *mapping = malloc(sizeof(sr_nat_mapping_t));
   
   mapping->aux_ext = htons(natNextMappingNumberAux(nat, type));
   mapping->conns = NULL;
   
   /* Update mapping information */
   mapping->aux_int = aux_int;
   mapping->ip_int = ip_int;
   mapping->last_updated = time(NULL);
   mapping->type = type;
   
   /* Add mapping to the front of the list. */
   mapping->next = nat->mappings;
   nat->mappings = mapping;
   
   return mapping;
}

static uint16_t natNextMappingNumberAux(sr_nat_t* nat, sr_nat_mapping_type type)
{
   uint16_t start;
   sr_nat_mapping_t *mappingWalker = nat->mappings;
   if (type == nat_mapping_icmp)
   {
      start = nat->nextIcmpIdentNumber;
   }
   else if (type == nat_mapping_tcp)
   {
      start = nat->nextTcpPortNumber;
   }
   
   /* if a mapping already exists for this port number */
   while (mappingWalker)
   {
      if ((mappingWalker->type == type) && (htons(start) == mappingWalker->aux_ext))
      {
         /* Mapping already exists. Go to the next. */
         start = (start == LAST_PORT_NUMBER) ? START_PORT_NUMBER : (start + 1);
         mappingWalker = nat->mappings;
      }
      else
      {
         mappingWalker = mappingWalker->next;
      }
   }
   
   if (type == nat_mapping_icmp)
   {
      nat->nextIcmpIdentNumber = (start == LAST_PORT_NUMBER) ? START_PORT_NUMBER : (start + 1);
   }
   else if (type == nat_mapping_tcp)
   {
      nat->nextTcpPortNumber = (start == LAST_PORT_NUMBER) ? START_PORT_NUMBER : (start + 1);
   }
   
   return start;
}

/* Receive Ip packet and NAT is enable. */
void natHandleRecievedIpPacket(struct sr_instance* sr, sr_ip_hdr_t* ipPacket, unsigned int length,
                                       sr_if_t *receivedInterface)
{
   if (ipPacket->ip_p == ip_protocol_tcp)
   {
      natHandleTcpPacket(sr, ipPacket, length, receivedInterface);
   }
   else if (ipPacket->ip_p == ip_protocol_icmp)
   {
      natHandleIcmpPacket(sr, ipPacket, length, receivedInterface);
   }
   else
   {
      /*Drop packet. */
   }
}

/*Function handle a TCP packet when NAT is enabled. */
static void natHandleTcpPacket(sr_instance_t* sr, sr_ip_hdr_t* ipPacket, unsigned int length,
   sr_if_t *receivedInterface)
{
   sr_tcp_hdr_t* tcpHeader = getTcpHeaderFromIpHeader(ipPacket);
   
   if (!TcpPerformIntegrityCheck(ipPacket, length))
   {
      return; /* Not pass IntegrityCheck. Drop. */
   }
   
   if ((getInternalInterface(sr)->ip == receivedInterface->ip) && (IpDestinationIsRouter(sr, ipPacket)))
   {
      HandleReceivedPacketToRouter(sr, ipPacket, length, receivedInterface);
   }
   else if (getInternalInterface(sr)->ip == receivedInterface->ip)
   {
      sr_nat_mapping_t *natMapping = sr_nat_lookup_internal(sr->nat, ipPacket->ip_src,
         tcpHeader->sourcePort, nat_mapping_tcp);
      
      if (ntohs(tcpHeader->offset_controlBits) & TCP_SYN)
      {
         if (natMapping == NULL)
         {
            /* Outbound SYN with no mapping. Create connection now.*/
            pthread_mutex_lock(&(sr->nat->lock));
            sr_nat_connection_t *firstConnection = malloc(sizeof(sr_nat_connection_t));
            sr_nat_mapping_t *sharedNatMapping;
            natMapping = malloc(sizeof(sr_nat_mapping_t));
            
            sharedNatMapping = natCreateMapping(sr->nat, ipPacket->ip_src,
               tcpHeader->sourcePort, nat_mapping_tcp);
            
            /* first connection information. */
            firstConnection->connectionState = nat_conn_outbound_syn;
            firstConnection->lastAccessed = time(NULL);
            firstConnection->queuedInboundSyn = NULL;
            firstConnection->external.ipAddress = ipPacket->ip_dst;
            firstConnection->external.portNumber = tcpHeader->destinationPort;
            
            /* Add to the list of connections. */
            firstConnection->next = sharedNatMapping->conns;
            sharedNatMapping->conns = firstConnection;
            
            memcpy(natMapping, sharedNatMapping, sizeof(sr_nat_mapping_t));
            
            pthread_mutex_unlock(&(sr->nat->lock));
         }
         else
         {
            /* Outbound SYN with previous mapping. Add the connection if one doesn't exist */
            pthread_mutex_lock(&(sr->nat->lock));
            sr_nat_mapping_t *sharedNatMapping = natLookupInternal(sr->nat, ipPacket->ip_src,
               tcpHeader->sourcePort, nat_mapping_tcp);
            
            sr_nat_connection_t *connection = natFindConnection(sharedNatMapping,
               ipPacket->ip_dst, tcpHeader->destinationPort);
            
            if (connection == NULL)
            {
               /* Connection does not exist. Create it. */
               connection = malloc(sizeof(sr_nat_connection_t));
               
               /* Connection information. */
               connection->connectionState = nat_conn_outbound_syn;
               connection->external.ipAddress = ipPacket->ip_dst;
               connection->external.portNumber = tcpHeader->destinationPort;
               
               /* Add to the list of connections. */
               connection->next = sharedNatMapping->conns;
               sharedNatMapping->conns = connection;
            }
            else if (connection->connectionState == nat_conn_time_wait)
            {
               connection->connectionState = nat_conn_outbound_syn;
            }
            else if (connection->connectionState == nat_conn_inbound_syn_pending)
            {
               connection->connectionState = nat_conn_connected;
               
               if (connection->queuedInboundSyn) { free(connection->queuedInboundSyn); }
            }
            pthread_mutex_unlock(&(sr->nat->lock));
         }
      }
      else if (natMapping == NULL)
      {
         return; /* Unopen connection. Drop packet. */
      }
      else if (ntohs(tcpHeader->offset_controlBits) & TCP_FIN)
      {
         /* Outbound FIN detected. */
         pthread_mutex_lock(&(sr->nat->lock));
         sr_nat_mapping_t *sharedNatMapping = natLookupInternal(sr->nat, ipPacket->ip_src,
            tcpHeader->sourcePort, nat_mapping_tcp);
         sr_nat_connection_t *correspondConnection = natFindConnection(sharedNatMapping, 
            ipPacket->ip_dst, tcpHeader->destinationPort);
         
         if (correspondConnection)
         {
            correspondConnection->connectionState = nat_conn_time_wait;
         }
         
         pthread_mutex_unlock(&(sr->nat->lock));
      }
      
      /* Forward packet. */
      natHandleReceivedOutboundIpPacket(sr, ipPacket, length, receivedInterface, natMapping);
      
      if (natMapping) 
      { free(natMapping); }
   }
   else /* Inbound TCP packet */
   {
      sr_nat_mapping_t *natMapping = sr_nat_lookup_external(sr->nat, tcpHeader->destinationPort,
         nat_mapping_tcp);
      
      if (ntohs(tcpHeader->offset_controlBits) & TCP_SYN)
      {
         /* Inbound SYN received. */
         if (natMapping == NULL)
         {
            IpSendTypeThreeIcmpPacket(sr, icmp_code_destination_port_unreachable, ipPacket);
            return;
         }
         else
         {
            /* Possible simultaneous open */
            pthread_mutex_lock(&(sr->nat->lock));
            
            sr_nat_mapping_t *sharedNatMapping = natLookupExternal(sr->nat, 
               tcpHeader->destinationPort, nat_mapping_tcp);
            
            sr_nat_connection_t *connection = natFindConnection(sharedNatMapping,
               ipPacket->ip_src, tcpHeader->sourcePort);
            
            if (connection == NULL)
            {
               /* Possible simultaneous open. */
               connection = malloc(sizeof(sr_nat_connection_t));
               
               /* Connection information. */
               connection->connectionState = nat_conn_inbound_syn_pending;
               connection->queuedInboundSyn = malloc(length);
               memcpy(connection->queuedInboundSyn, ipPacket, length);
               connection->external.ipAddress = ipPacket->ip_src;
               connection->external.portNumber = tcpHeader->sourcePort;
               
               /* Add to the list of connections. */
               connection->next = sharedNatMapping->conns;
               sharedNatMapping->conns = connection;
               
               return;
            }
            else if (connection->connectionState == nat_conn_inbound_syn_pending)
            {
               return; /* Drop.*/
            }
            else if (connection->connectionState == nat_conn_outbound_syn)
            {
               connection->connectionState = nat_conn_connected;
            }
            pthread_mutex_unlock(&(sr->nat->lock));
         }
      }
      else if (natMapping == NULL)
      {
         IpSendTypeThreeIcmpPacket(sr, icmp_code_destination_port_unreachable, ipPacket);
         return;
      }
      else if (ntohs(tcpHeader->offset_controlBits) & TCP_FIN)
      {
         pthread_mutex_lock(&(sr->nat->lock));
         sr_nat_mapping_t *sharedNatMapping = natLookupExternal(sr->nat, 
            tcpHeader->destinationPort, nat_mapping_tcp);
         sr_nat_connection_t *correspondConnection = natFindConnection(sharedNatMapping, 
            ipPacket->ip_src, tcpHeader->sourcePort);
         
         if (correspondConnection)
         {
            correspondConnection->connectionState = nat_conn_time_wait;
         }
         
         pthread_mutex_unlock(&(sr->nat->lock));
      }
      else
      {
         pthread_mutex_lock(&(sr->nat->lock));
         sr_nat_mapping_t *sharedNatMapping = natLookupExternal(sr->nat, 
            tcpHeader->destinationPort, nat_mapping_tcp);
         sr_nat_connection_t *correspondConnection = natFindConnection(sharedNatMapping, 
            ipPacket->ip_src, tcpHeader->sourcePort);
         
         if (correspondConnection == NULL)
         {
            /* Received unsolicited non-SYN packet when no connection was found */
            pthread_mutex_unlock(&(sr->nat->lock));  
            return;
         }
         else
         {
            pthread_mutex_unlock(&(sr->nat->lock));
         }
      }
      
      natHandleReceivedInboundIpPacket(sr, ipPacket, length, receivedInterface, natMapping);
      
      if (natMapping) { free(natMapping); }
   }
}

/* Finds the correspond TCP connection in a NAT mapping given an external (IP,Port) pair.*/
static sr_nat_connection_t *natFindConnection(sr_nat_mapping_t *natEntry, uint32_t ip_ext, 
   uint16_t port_ext)
{
   sr_nat_connection_t *connectionWalker = natEntry->conns;
   while (connectionWalker != NULL)
   {
      if ((connectionWalker->external.ipAddress == ip_ext) 
         && (connectionWalker->external.portNumber == port_ext))
      {
         connectionWalker->lastAccessed = time(NULL);
         break;
      }
      
      connectionWalker = connectionWalker->next;
   }
   return connectionWalker;
}

/* Perform NAT translatation and Forward packet (Outbound). */
static void natHandleReceivedOutboundIpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet,
   unsigned int length,struct sr_if* receivedInterface, sr_nat_mapping_t *natMapping)
{
   if (packet->ip_p == ip_protocol_icmp)
   {
      sr_icmp_hdr_t *icmpPacketHeader = (sr_icmp_hdr_t *) (((uint8_t*) packet)
                                       + getIpHeaderLength(packet));

      if ((icmpPacketHeader->icmp_type == icmp_type_echo_request)
         || (icmpPacketHeader->icmp_type == icmp_type_echo_reply))
      {
         sr_icmp_t0_hdr_t* rewrittenIcmpHeader = (sr_icmp_t0_hdr_t*) icmpPacketHeader;
         int icmpLength = length - getIpHeaderLength(packet);
                  
         /* Handle ICMP identify */
         rewrittenIcmpHeader->ident = natMapping->aux_ext;
         rewrittenIcmpHeader->icmp_sum = 0;
         rewrittenIcmpHeader->icmp_sum = cksum(rewrittenIcmpHeader, icmpLength);
         
         /* Handle IP address */
         packet->ip_src = sr_get_interface(sr,
            getLongestPrefixRoute(sr, ntohl(packet->ip_dst))->interface)->ip;
         
         ForwardIpPacket(sr, packet, length, receivedInterface);
      }
      else
      {
         int icmpLength = length - getIpHeaderLength(packet);
         sr_ip_hdr_t *originalPacket;
         if (icmpPacketHeader->icmp_type == icmp_type_desination_unreachable)
         {
            sr_icmp_t3_hdr_t *unreachableICMP = (sr_icmp_t3_hdr_t *) icmpPacketHeader;
            originalPacket = (sr_ip_hdr_t*) (unreachableICMP->data);
         }
         else if (icmpPacketHeader->icmp_type == icmp_type_time_exceeded)
         {
            sr_icmp_t11_hdr_t *unreachableICMP = (sr_icmp_t11_hdr_t *) icmpPacketHeader;
            originalPacket = (sr_ip_hdr_t*) (unreachableICMP->data);
         }
                     
         if (originalPacket->ip_p == ip_protocol_tcp)
         {
            sr_tcp_hdr_t *originalHeader = getTcpHeaderFromIpHeader(originalPacket);
            
            /* Perform mapping on extra payload (first 8 bytes) */
            originalHeader->destinationPort = natMapping->aux_ext;
            originalPacket->ip_dst = sr_get_interface(sr,
               getLongestPrefixRoute(sr, ntohl(packet->ip_dst))->interface)->ip;
         }
         else if (originalPacket->ip_p == ip_protocol_icmp)
         {
            sr_icmp_t0_hdr_t *originalHeader =
               (sr_icmp_t0_hdr_t *) getIcmpHeaderFromIpHeader(originalPacket);
            
            /* Perform mapping on extra payload (first 8 bytes)*/
            originalHeader->ident = natMapping->aux_ext;
            originalPacket->ip_dst = sr_get_interface(sr,
               getLongestPrefixRoute(sr, ntohl(packet->ip_dst))->interface)->ip;
         }
         
         /* Update ICMP checksum */
         icmpPacketHeader->icmp_sum = 0;
         icmpPacketHeader->icmp_sum = cksum(icmpPacketHeader, icmpLength);
         
         /* Rewrite actual packet header. */
         packet->ip_src = sr_get_interface(sr,
            getLongestPrefixRoute(sr, ntohl(packet->ip_dst))->interface)->ip;
         
         ForwardIpPacket(sr, packet, length, receivedInterface);
      }
   }
   else if (packet->ip_p == ip_protocol_tcp)
   {
      sr_tcp_hdr_t* tcpHeader = (sr_tcp_hdr_t *) (((uint8_t*) packet) + getIpHeaderLength(packet));
      
      tcpHeader->sourcePort = natMapping->aux_ext;
      packet->ip_src = sr_get_interface(sr, getLongestPrefixRoute(sr, ntohl(packet->ip_dst))->interface)->ip;
      
      natRecalculateTcpChecksum(packet, length);
      ForwardIpPacket(sr, packet, length, receivedInterface);
   }
}

/* NAT translation, and forwards the packet (Inbound). */
static void natHandleReceivedInboundIpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet, 
   unsigned int length, struct sr_if* receivedInterface, sr_nat_mapping_t *natMapping)
{
   if (packet->ip_p == ip_protocol_icmp)
   {
      sr_icmp_hdr_t *icmpPacketHeader = getIcmpHeaderFromIpHeader(packet);
      
      if ((icmpPacketHeader->icmp_type == icmp_type_echo_request)
         || (icmpPacketHeader->icmp_type == icmp_type_echo_reply))
      {
         sr_icmp_t0_hdr_t *echoPacketHeader = (sr_icmp_t0_hdr_t *) icmpPacketHeader;
         int icmpLength = length - getIpHeaderLength(packet);
                  
         /* Handle ICMP rewrite. */
         echoPacketHeader->ident = natMapping->aux_int;
         echoPacketHeader->icmp_sum = 0;
         echoPacketHeader->icmp_sum = cksum(echoPacketHeader, icmpLength);
         
         /* Handle IP header rewrite. */
         packet->ip_dst = natMapping->ip_int;
         
         ForwardIpPacket(sr, packet, length, receivedInterface);
      }
      else 
      {
         int icmpLength = length - getIpHeaderLength(packet);
         sr_ip_hdr_t * originalPacket;
         if (icmpPacketHeader->icmp_type == icmp_type_desination_unreachable)
         {
            sr_icmp_t3_hdr_t *unreachableICMP = (sr_icmp_t3_hdr_t *) icmpPacketHeader;
            originalPacket = (sr_ip_hdr_t*) (unreachableICMP->data);
         }
         else if (icmpPacketHeader->icmp_type == icmp_type_time_exceeded)
         {
            sr_icmp_t11_hdr_t *unreachableICMP = (sr_icmp_t11_hdr_t *) icmpPacketHeader;
            originalPacket = (sr_ip_hdr_t*) (unreachableICMP->data);
         }
            
         assert(natMapping);
         
         if (originalPacket->ip_p == ip_protocol_tcp)
         {
            sr_tcp_hdr_t *originalHeader = getTcpHeaderFromIpHeader(originalPacket);
            
            /* Perform mapping on extra payload (first 8 bytes)*/
            originalHeader->sourcePort = natMapping->aux_int;
            originalPacket->ip_src = natMapping->ip_int;
         }
         else if (originalPacket->ip_p == ip_protocol_icmp)
         {
            sr_icmp_t0_hdr_t *originalHeader =
               (sr_icmp_t0_hdr_t *) getIcmpHeaderFromIpHeader(originalPacket);
            
            /* Perform mapping on extra payload (first 8 bytes)*/
            originalHeader->ident = natMapping->aux_int;
            originalPacket->ip_src = natMapping->ip_int;
         }
         
         /* Update ICMP checksum */
         icmpPacketHeader->icmp_sum = 0;
         icmpPacketHeader->icmp_sum = cksum(icmpPacketHeader, icmpLength);
         
         /* Rewrite actual packet header. */
         packet->ip_dst = natMapping->ip_int;
         
         ForwardIpPacket(sr, packet, length, receivedInterface);
      }
   }
   else if (packet->ip_p == ip_protocol_tcp)
   {
      sr_tcp_hdr_t* tcpHeader = (sr_tcp_hdr_t *) (((uint8_t*) packet) + getIpHeaderLength(packet));
            
      tcpHeader->destinationPort = natMapping->aux_int;
      packet->ip_dst = natMapping->ip_int;
      
      natRecalculateTcpChecksum(packet, length);
      ForwardIpPacket(sr, packet, length, receivedInterface);
   }
}

/*Function handle an ICMP packet when NAT is enabled. */
static void natHandleIcmpPacket(sr_instance_t* sr, sr_ip_hdr_t* ipPacket, unsigned int length,
   sr_if_t *receivedInterface)
{
   sr_icmp_hdr_t * icmpHeader = getIcmpHeaderFromIpHeader(ipPacket);
   
   if (!IcmpPerformIntegrityCheck(icmpHeader, length - getIpHeaderLength(ipPacket)))
   {
      return; /* Drop.*/
   }
   
   if ((getInternalInterface(sr)->ip == receivedInterface->ip) && (IpDestinationIsRouter(sr, ipPacket)))
   {
      HandleReceivedPacketToRouter(sr, ipPacket, length, receivedInterface);
   }
   else if (getInternalInterface(sr)->ip == receivedInterface->ip)
   {
      if ((icmpHeader->icmp_type == icmp_type_echo_request)
         || (icmpHeader->icmp_type == icmp_type_echo_reply))
      {
         sr_icmp_t0_hdr_t *icmpPingHdr = (sr_icmp_t0_hdr_t *) icmpHeader;
         sr_nat_mapping_t *natlookupInternalResult = sr_nat_lookup_internal(sr->nat, ipPacket->ip_src, 
            icmpPingHdr->ident, nat_mapping_icmp);
         
         if (natlookupInternalResult == NULL) /* No mapping. */
         {
            natlookupInternalResult = sr_nat_insert_mapping(sr->nat, ipPacket->ip_src, icmpPingHdr->ident,
               nat_mapping_icmp);
         }
         
         natHandleReceivedOutboundIpPacket(sr, ipPacket, length, receivedInterface, natlookupInternalResult);
         free(natlookupInternalResult);
      }
      else 
      {
         sr_ip_hdr_t *IpPacket = NULL;
         sr_nat_mapping_t *natlookupInternalResult = NULL;
         
         if (icmpHeader->icmp_type == icmp_type_desination_unreachable)
         {
            sr_icmp_t3_hdr_t *unreachableHeader = (sr_icmp_t3_hdr_t *) icmpHeader;
            IpPacket = (sr_ip_hdr_t *) unreachableHeader->data;
         }
         else if (icmpHeader->icmp_type == icmp_type_time_exceeded)
         {
            sr_icmp_t11_hdr_t *timeExceededHeader = (sr_icmp_t11_hdr_t *) icmpHeader;
            IpPacket = (sr_ip_hdr_t *) timeExceededHeader->data;
         }
         else
         {
            return;
         }
                  
         if (IpPacket->ip_p == ip_protocol_icmp)
         {
            sr_icmp_t0_hdr_t * extraIcmpHeader =
               (sr_icmp_t0_hdr_t *) getIcmpHeaderFromIpHeader(IpPacket);
            if ((extraIcmpHeader->icmp_type == icmp_type_echo_request)
               || (extraIcmpHeader->icmp_type == icmp_type_echo_reply))
            {
               natlookupInternalResult = sr_nat_lookup_internal(sr->nat, IpPacket->ip_dst, 
                  extraIcmpHeader->ident, nat_mapping_icmp);
            }
         }
         else if (IpPacket->ip_p == ip_protocol_tcp)
         {
            sr_tcp_hdr_t *extraTcpHeader = getTcpHeaderFromIpHeader(IpPacket);
            natlookupInternalResult = sr_nat_lookup_internal(sr->nat, IpPacket->ip_dst,
               extraTcpHeader->destinationPort, nat_mapping_tcp);
         }
         else
         {
            return;
         }
         
         if (natlookupInternalResult != NULL)
         {
            natHandleReceivedOutboundIpPacket(sr, ipPacket, length, receivedInterface,
               natlookupInternalResult);
            free(natlookupInternalResult);
         }
      }
   }
   else /* Inbound ICMP packet */
   {
      if (!IpDestinationIsRouter(sr, ipPacket))
      {
         if (getInternalInterface(sr)->ip
            != sr_get_interface(sr, getLongestPrefixRoute(sr, ntohl(ipPacket->ip_dst))->interface)->ip)
         {
            ForwardIpPacket(sr, ipPacket, length, receivedInterface);
         }
         else
         {
            fprintf(stderr, "Unsolicited inbound ICMP packet received. Dropping.\n");
         }
         return;
      }
      else if (ipPacket->ip_dst == getInternalInterface(sr)->ip)
      {
         fprintf(stderr, "Received ICMP packet to internal interface. Dropping.\n");
         return;
      }
      else if ((icmpHeader->icmp_type == icmp_type_echo_request)
         || (icmpHeader->icmp_type == icmp_type_echo_reply))
      {
         sr_icmp_t0_hdr_t *icmpPingHdr = (sr_icmp_t0_hdr_t *) icmpHeader;
         sr_nat_mapping_t *natlookupExternalResult = sr_nat_lookup_external(sr->nat, icmpPingHdr->ident, 
            nat_mapping_icmp);
         
         if (natlookupExternalResult == NULL)
         {
            /* No mapping exists.*/
            HandleReceivedPacketToRouter(sr, ipPacket, length, receivedInterface);
         }
         else
         {
            natHandleReceivedInboundIpPacket(sr, ipPacket, length, receivedInterface,
               natlookupExternalResult);
            free (natlookupExternalResult);
         }
      }
      else 
      {
         sr_ip_hdr_t *IpPacket = NULL;
         sr_nat_mapping_t *natlookupExternalResult = NULL;
         
         if (icmpHeader->icmp_type == icmp_type_desination_unreachable)
         {
            sr_icmp_t3_hdr_t *unreachableHeader = (sr_icmp_t3_hdr_t *) icmpHeader;
            IpPacket = (sr_ip_hdr_t *) unreachableHeader->data;
         }
         else if (icmpHeader->icmp_type == icmp_type_time_exceeded)
         {
            sr_icmp_t11_hdr_t *timeExceededHeader = (sr_icmp_t11_hdr_t *) icmpHeader;
            IpPacket = (sr_ip_hdr_t *) timeExceededHeader->data;
         }
         else
         {
            return; /* Unsolicited packet received.*/
         }
                  
         if (IpPacket->ip_p == ip_protocol_icmp)
         {
            sr_icmp_t0_hdr_t * extraIcmpHeader =
               (sr_icmp_t0_hdr_t *) getIcmpHeaderFromIpHeader(IpPacket);
            if ((extraIcmpHeader->icmp_type == icmp_type_echo_request)
               || (extraIcmpHeader->icmp_type == icmp_type_echo_reply))
            {
               natlookupExternalResult = sr_nat_lookup_external(sr->nat, extraIcmpHeader->ident, 
                  nat_mapping_icmp);
            }
         }
         else if (IpPacket->ip_p == ip_protocol_tcp)
         {
            sr_tcp_hdr_t * extraTcpHeader = getTcpHeaderFromIpHeader(IpPacket);
            natlookupExternalResult = sr_nat_lookup_external(sr->nat, extraTcpHeader->sourcePort, nat_mapping_tcp);
         }
         else
         {
            return; /* Drop.*/
         }
         
         if (natlookupExternalResult != NULL)
         {
            natHandleReceivedInboundIpPacket(sr, ipPacket, length, receivedInterface,
               natlookupExternalResult);
            free(natlookupExternalResult);
         }
      }
   }
}

/* Receives Ip packet and NAT handle not done (time exceed)*/
void natNotdonePacketMapping(struct sr_instance* sr, sr_ip_hdr_t* ipDatagram, unsigned int length, 
  sr_if_t *receivedInterface)
{
   sr_nat_mapping_t *natMapping;
   if (getInternalInterface(sr)->ip == receivedInterface->ip)
   {
      /* Outbound.*/
      if (ipDatagram->ip_p == ip_protocol_icmp)
      {
         sr_icmp_t0_hdr_t *icmpHeader = (sr_icmp_t0_hdr_t *)getIcmpHeaderFromIpHeader(ipDatagram);
         natMapping = sr_nat_lookup_external(sr->nat, icmpHeader->ident, nat_mapping_icmp);
         if (natMapping != NULL)
         {
            icmpHeader->ident = natMapping->aux_int;
            icmpHeader->icmp_sum = 0;
            icmpHeader->icmp_sum = cksum(icmpHeader, length - getIpHeaderLength(ipDatagram));
            
            ipDatagram->ip_src = natMapping->ip_int;
            ipDatagram->ip_sum = 0;
            ipDatagram->ip_sum = cksum(ipDatagram, getIpHeaderLength(ipDatagram));
         }
         free(natMapping);
      }
      else if (ipDatagram->ip_p == ip_protocol_tcp)
      {
         sr_tcp_hdr_t *tcpHeader = getTcpHeaderFromIpHeader(ipDatagram);
         natMapping = sr_nat_lookup_external(sr->nat, tcpHeader->sourcePort, nat_mapping_tcp);
         if (natMapping != NULL)
         {
            tcpHeader->sourcePort = natMapping->aux_int;
            ipDatagram->ip_src = natMapping->ip_int;
            
            natRecalculateTcpChecksum(ipDatagram, length);
            
            ipDatagram->ip_sum = 0;
            ipDatagram->ip_sum = cksum(ipDatagram, getIpHeaderLength(ipDatagram));
         }
         free(natMapping);
      }
   }
   else
   {
      /* Inbound. */
      if (ipDatagram->ip_p == ip_protocol_icmp)
      {
         sr_icmp_t0_hdr_t *icmpHeader = (sr_icmp_t0_hdr_t *) (((uint8_t *) ipDatagram)
                                          + getIpHeaderLength(ipDatagram));
         natMapping = sr_nat_lookup_internal(sr->nat, ntohl(ipDatagram->ip_dst), 
            ntohs(icmpHeader->ident), nat_mapping_icmp);
         if (natMapping != NULL)
         {
            icmpHeader->ident = htons(natMapping->aux_ext);
            icmpHeader->icmp_sum = 0;
            icmpHeader->icmp_sum = cksum(icmpHeader, length - getIpHeaderLength(ipDatagram));
            
            ipDatagram->ip_dst = sr_get_interface(sr,
               getLongestPrefixRoute(sr, ipDatagram->ip_src)->interface)->ip;
            ipDatagram->ip_sum = 0;
            ipDatagram->ip_sum = cksum(ipDatagram, getIpHeaderLength(ipDatagram));
            
            free(natMapping);
         }
      }
      else if (ipDatagram->ip_p == ip_protocol_tcp)
      {
         sr_tcp_hdr_t *tcpHeader = (sr_tcp_hdr_t *) (((uint8_t *)ipDatagram)
                                    + getIpHeaderLength(ipDatagram));
         natMapping = sr_nat_lookup_internal(sr->nat, ntohl(ipDatagram->ip_dst), 
            ntohs(tcpHeader->destinationPort), nat_mapping_icmp);
         if (natMapping != NULL)
         {
            tcpHeader->destinationPort = htons(natMapping->aux_ext);
            ipDatagram->ip_dst = sr_get_interface(sr,
               getLongestPrefixRoute(sr, ipDatagram->ip_src)->interface)->ip;
            
            natRecalculateTcpChecksum(ipDatagram, length);
            
            ipDatagram->ip_sum = 0;
            ipDatagram->ip_sum = cksum(ipDatagram, getIpHeaderLength(ipDatagram));
            
            free(natMapping);
         }
      }
   }
}

/* Destroys a specified connection in the specified natMapping. */
static void sr_nat_destroy_connection(sr_nat_mapping_t* natMapping, sr_nat_connection_t* connection)
{
   sr_nat_connection_t *req, *prev = NULL, *next = NULL;
   
   if (natMapping && connection)
   {
      for (req = natMapping->conns; req != NULL; req = req->next)
      {
         if (req == connection)
         {
            if (prev)
            {
               next = req->next;
               prev->next = next;
            }
            else
            {
               next = req->next;
               natMapping->conns = next;
            }
            
            break;
         }
         prev = req;
      }
      
      if(connection->queuedInboundSyn)
      {
         free(connection->queuedInboundSyn);
      }
      
      free(connection);
   }
}

/* Recalculating a TCP packet checksum after it has been changed.*/
static void natRecalculateTcpChecksum(sr_ip_hdr_t *tcpPacket, unsigned int length)
{
   unsigned int tcpLength = length - getIpHeaderLength(tcpPacket);
   uint8_t *packetCopy = malloc(sizeof(sr_tcp_ip_pseudo_hdr_t) + tcpLength);
   sr_tcp_ip_pseudo_hdr_t *reChecksumHeader = (sr_tcp_ip_pseudo_hdr_t *) packetCopy;
   sr_tcp_hdr_t *tcpHeader = (sr_tcp_hdr_t *) (((uint8_t*) tcpPacket)
      + getIpHeaderLength(tcpPacket));
   
   memcpy(packetCopy + sizeof(sr_tcp_ip_pseudo_hdr_t), tcpHeader, tcpLength);
   reChecksumHeader->sourceAddress = tcpPacket->ip_src;
   reChecksumHeader->destinationAddress = tcpPacket->ip_dst;
   reChecksumHeader->reserved = 0;
   reChecksumHeader->protocol = ip_protocol_tcp;
   reChecksumHeader->tcpLength = htons(tcpLength);
   
   tcpHeader->checksum = 0;
   tcpHeader->checksum = cksum(packetCopy, sizeof(sr_tcp_ip_pseudo_hdr_t) + tcpLength);
   
   free(packetCopy);
}