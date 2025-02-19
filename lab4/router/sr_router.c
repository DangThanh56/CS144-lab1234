/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"

#define MIN_IP_HEADER_LEN  5
#define DEFAULT_TTL           64
#define IP_VERSION  4
/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/
static uint16_t ipIdentifyNumber = 0;
 
static const uint8_t broadcast[ETHER_ADDR_LEN] =
   { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static bool natEnabled(sr_instance_t *sr)
{
  return (sr->nat != NULL);
}
static uint16_t getIpHeaderlen(sr_ip_hdr_t *pkt)
{
  return (pkt->ip_hl) * 4;
}

/* Add function. */
void SendArpRequest(struct sr_instance* sr, struct sr_arpreq* request); 
void IpSendTypeThreeIcmpPacket(struct sr_instance* sr, sr_icmp_code_t icmpCode,
                                                 sr_ip_hdr_t* originalPacket);
void HandleReceivedPacketToRouter(struct sr_instance* sr, sr_ip_hdr_t* packet,
                                     unsigned int len, sr_if_t *interface);
void ForwardIpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet,
                  unsigned int len, struct sr_if* receivedInterface); 
sr_rt_t* getLongestPrefixRoute(struct sr_instance* sr, in_addr_t destIp);
bool IcmpPerformIntegrityCheck(sr_icmp_hdr_t *icmpPacket, unsigned int len);
bool TcpPerformIntegrityCheck(sr_ip_hdr_t *tcpPacket, unsigned int len);
bool IpDestinationIsRouter(struct sr_instance* sr, sr_ip_hdr_t* packet);

static void HandleReceivedArpPacket(struct sr_instance* sr, sr_arp_hdr_t * packet,
                                    unsigned int len, struct sr_if* interface);
static void HandleReceivedIpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet,
                                 unsigned int len, struct sr_if* interface);
static void HandleIcmpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet,
                                 unsigned int len, struct sr_if* interface);
static void SendIcmpEchoReply(struct sr_instance* sr, sr_ip_hdr_t* echoRequestPacket,
                                                         unsigned int len);                                 
static void SendIcmpTimeExceed(struct sr_instance* sr, sr_ip_hdr_t* originalPacket,
                              unsigned int len, sr_if_t *receivedInterface);
static void handleArpAndSendPacket(sr_instance_t *sr, sr_ethernet_hdr_t* packet, 
                                           unsigned int len, sr_rt_t* route);
static bool IpSourceForRouter(struct sr_instance* sr, sr_ip_hdr_t* packet);
static int getLongestPrefixlen(uint32_t mask);

void sr_init(struct sr_instance* sr)
{
    /* REQUIRES */
    assert(sr);

    /* Initialize cache and cache cleanup thread */
    sr_arpcache_init(&(sr->cache));

    pthread_attr_init(&(sr->attr));
    pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
    pthread_t thread;

    pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);
    
    /* Add initialization code here! */

} /* -- sr_init -- */

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet len and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance* sr,
        uint8_t * packet/* lent */,
        unsigned int len,
        char* interface/* lent */)
{
  /* REQUIRES */
  assert(sr);
  assert(packet);
  assert(interface);

  printf("*** -> Received packet of len %d \n",len);

  /* fill in code here */
  struct sr_if* receivedInterface = NULL;

  if (len < sizeof(sr_ethernet_hdr_t)) { return; }

  receivedInterface = sr_get_interface(sr, interface);
  
  if ((receivedInterface == NULL)
    || ((memcmp(((sr_ethernet_hdr_t*)packet)->ether_dhost, receivedInterface->addr, ETHER_ADDR_LEN) != 0)
        && (memcmp(((sr_ethernet_hdr_t*)packet)->ether_dhost, broadcast, ETHER_ADDR_LEN) != 0)))
  {
    return; /* Drop*/
  }
  
  switch (ethertype(packet))
  {
    case ethertype_arp:
        HandleReceivedArpPacket(sr, (sr_arp_hdr_t*) (packet + sizeof(sr_ethernet_hdr_t)),
          len - sizeof(sr_ethernet_hdr_t), receivedInterface);
        break;

    case ethertype_ip:
        HandleReceivedIpPacket(sr, (sr_ip_hdr_t*) (packet + sizeof(sr_ethernet_hdr_t)),
          len - sizeof(sr_ethernet_hdr_t), receivedInterface);
        break;

    default:
        return; /* Drop*/
  }  
}/* end sr_ForwardPacket */

/* Function sends an ARP request based on the provided request */
void SendArpRequest(struct sr_instance* sr, struct sr_arpreq* request)
{
   uint8_t* arpPacket = (uint8_t *) malloc(sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t));
   sr_ethernet_hdr_t* ethernetHdr = (sr_ethernet_hdr_t*) arpPacket;
   sr_arp_hdr_t* arpHdr = (sr_arp_hdr_t*) (arpPacket + sizeof(sr_ethernet_hdr_t));
   
   /* Ethernet Header */
   memcpy(ethernetHdr->ether_dhost, broadcast, ETHER_ADDR_LEN);
   memcpy(ethernetHdr->ether_shost, request->requestedInterface->addr, ETHER_ADDR_LEN);
   ethernetHdr->ether_type = htons(ethertype_arp);
   
   /* ARP Header */
   arpHdr->ar_hrd = htons(arp_hrd_ethernet);
   arpHdr->ar_pro = htons(ethertype_ip);
   arpHdr->ar_hln = ETHER_ADDR_LEN;
   arpHdr->ar_pln = IP_ADDR_LEN;
   arpHdr->ar_op = htons(arp_op_request);
   memcpy(arpHdr->ar_sha, request->requestedInterface->addr, ETHER_ADDR_LEN);
   arpHdr->ar_sip = request->requestedInterface->ip;
   memset(arpHdr->ar_tha, 0, ETHER_ADDR_LEN); /* Follow by RFC 826 */
   arpHdr->ar_tip = htonl(request->ip);
   
   /* Send ARP packet */
   sr_send_packet(sr, arpPacket, sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t),
      request->requestedInterface->name);
   
   free(arpPacket);
}
 
/* Function sends a type 3 (Destination Unreachable) packet */
void IpSendTypeThreeIcmpPacket(struct sr_instance* sr, sr_icmp_code_t icmpCode,
   sr_ip_hdr_t* originalPacket)
{
   struct sr_rt* icmpRoute;
   struct sr_if* destinationInterface;
   
   uint8_t* replyPacket = malloc(sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) 
      + sizeof(sr_icmp_t3_hdr_t));
   sr_ip_hdr_t* replyIpHeader = (sr_ip_hdr_t*) (replyPacket + sizeof(sr_ethernet_hdr_t));
   sr_icmp_t3_hdr_t* replyIcmpHeader = (sr_icmp_t3_hdr_t*) ((uint8_t*) replyIpHeader
      + sizeof(sr_ip_hdr_t));
   
   if (IpSourceForRouter(sr, originalPacket))
   {
      free(replyPacket);
      return;
   }
   
   /* IP header */
   replyIpHeader->ip_v = IP_VERSION;
   replyIpHeader->ip_hl = MIN_IP_HEADER_LEN;
   replyIpHeader->ip_tos = 0;
   replyIpHeader->ip_len = htons(sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
   replyIpHeader->ip_id = htons(ipIdentifyNumber); ipIdentifyNumber++;
   replyIpHeader->ip_off = htons(IP_DF);
   replyIpHeader->ip_ttl = DEFAULT_TTL;
   replyIpHeader->ip_p = ip_protocol_icmp;
   replyIpHeader->ip_sum = 0;
   replyIpHeader->ip_dst = originalPacket->ip_src; /* Network byte order. */
   
   icmpRoute = getLongestPrefixRoute(sr, ntohl(replyIpHeader->ip_dst));
   destinationInterface = sr_get_interface(sr, icmpRoute->interface);
   
   replyIpHeader->ip_src = destinationInterface->ip;
   replyIpHeader->ip_sum = cksum(replyIpHeader, getIpHeaderLength(replyIpHeader));
   
   /* ICMP header */
   replyIcmpHeader->icmp_type = icmp_type_desination_unreachable;
   replyIcmpHeader->icmp_code = icmpCode;
   replyIcmpHeader->icmp_sum = 0;
   memcpy(replyIcmpHeader->data, originalPacket, ICMP_DATA_SIZE);
   replyIcmpHeader->icmp_sum = cksum(replyIcmpHeader, sizeof(sr_icmp_t3_hdr_t));
   
   handleArpAndSendPacket(sr, (sr_ethernet_hdr_t*) replyPacket,
      sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t),
      getLongestPrefixRoute(sr, ntohl(replyIpHeader->ip_dst)));
   
   free(replyPacket);
}
 
/* Function handles a received IP packet destined for the router */
void HandleReceivedPacketToRouter(struct sr_instance* sr, sr_ip_hdr_t* packet,
   unsigned int length, sr_if_t* interface)
{
   if (packet->ip_p == (uint8_t) ip_protocol_icmp)
   {
      HandleIcmpPacket(sr, packet, length, interface);
   }
   else
   {
      /* Packet contain TCP/UDP payload. Send port unreachable.*/
      IpSendTypeThreeIcmpPacket(sr, icmp_code_destination_port_unreachable, packet);
   }
}

/* Function forward packet to next hop.*/
void ForwardIpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet,
   unsigned int length, struct sr_if* receivedInterface)
{
   struct sr_rt* forwardRoute = getLongestPrefixRoute(sr, ntohl(packet->ip_dst));
   /* Decrement TTL and forward. */
   uint8_t packetTtl = packet->ip_ttl - 1;
   if (packetTtl == 0)
   {
   /* Run out of time */
      SendIcmpTimeExceed(sr, packet, length, receivedInterface);
      return;
   }
   else
   {
      /* Recalculate checksum since we has changed the packet header (ttl). */
      packet->ip_ttl = packetTtl;
      packet->ip_sum = 0;
      packet->ip_sum = cksum(packet, getIpHeaderLength(packet));
   }
   
   if (forwardRoute != NULL)
   {
      /* Found a route. Forward packet! */
      uint8_t* forwardPacket = malloc(length + sizeof(sr_ethernet_hdr_t));
      memcpy(forwardPacket + sizeof(sr_ethernet_hdr_t), packet, length);
      
      /* Handle ARP and send packet.*/
      handleArpAndSendPacket(sr, (sr_ethernet_hdr_t*)forwardPacket,
         length + sizeof(sr_ethernet_hdr_t), forwardRoute);
      
      free(forwardPacket);
   }
   else
   {
      /* No found route. Send icmp network unreachable.*/
      IpSendTypeThreeIcmpPacket(sr, icmp_code_network_unreachable, packet);
   }
}
 
/* Function gets the longest prefix match route for a provided destination IP address */
sr_rt_t* getLongestPrefixRoute(struct sr_instance* sr, in_addr_t destIp)
{
   struct sr_rt* routeWalker;
   int getMaskLength = -1;
   struct sr_rt* ret = NULL;
   
   for (routeWalker = sr->routing_table; routeWalker; routeWalker = routeWalker->next)
   {
      if (getLongestPrefixLength(routeWalker->mask.s_addr) > getMaskLength)
      {
         if ((destIp & routeWalker->mask.s_addr) 
            == (ntohl(routeWalker->dest.s_addr) & routeWalker->mask.s_addr))
         {
            /* Longer prefix match found. */
            ret = routeWalker;
            getMaskLength = getLongestPrefixLength(routeWalker->mask.s_addr);
         }
      }
   }
   
   return ret;
}
 
/* Performs the ICMP checksum on a received ICMP packet */
bool IcmpPerformIntegrityCheck(sr_icmp_hdr_t *icmpPacket, unsigned int length)
{
   /* Check the integrity of the ICMP packet */
   uint16_t headerChecksum = icmpPacket->icmp_sum;
   uint16_t calculatedChecksum = 0;
   icmpPacket->icmp_sum = 0;
   
   calculatedChecksum = cksum(icmpPacket, length);
   icmpPacket->icmp_sum = headerChecksum;
   
   if (headerChecksum != calculatedChecksum)
   {
      return false; /* Checksum fail.*/
   }
   return true;
}
 
/* Performs the TCP checksum on a received TCP packet */
bool TcpPerformIntegrityCheck(sr_ip_hdr_t *tcpPacket, unsigned int length)
{
   bool ret;
   unsigned int tcpLength = length - getIpHeaderLength(tcpPacket);
   uint8_t *packetCopy = malloc(sizeof(sr_tcp_ip_pseudo_hdr_t) + tcpLength);
   sr_tcp_ip_pseudo_hdr_t * checksumHeader = (sr_tcp_ip_pseudo_hdr_t *) packetCopy;
   sr_tcp_hdr_t *tcpHeader = (sr_tcp_hdr_t *) (((uint8_t*) tcpPacket)
      + getIpHeaderLength(tcpPacket));
   
   uint16_t calculatedChecksum = 0;
   uint16_t headerChecksum = tcpHeader->checksum;
   tcpHeader->checksum = 0;
   
   memcpy(packetCopy + sizeof(sr_tcp_ip_pseudo_hdr_t), tcpHeader, tcpLength);
   checksumHeader->sourceAddress = tcpPacket->ip_src;
   checksumHeader->destinationAddress = tcpPacket->ip_dst;
   checksumHeader->reserved = 0;
   checksumHeader->protocol = ip_protocol_tcp;
   checksumHeader->tcpLength = htons(tcpLength);
   
   calculatedChecksum = cksum(packetCopy, sizeof(sr_tcp_ip_pseudo_hdr_t) + tcpLength);
   
   ret = (headerChecksum == calculatedChecksum) ? true : false; 
   
   free(packetCopy);
   
   return ret;
}
 
/* Function checks if ANY of our IP addresses matches the packet destination IP */
bool IpDestinationIsRouter(struct sr_instance* sr, sr_ip_hdr_t* packet)
{
   struct sr_if* interfaceWalker;
   
   for (interfaceWalker = sr->if_list; interfaceWalker != NULL; interfaceWalker =
      interfaceWalker->next)
   {
      if (packet->ip_dst == interfaceWalker->ip)
      {
         return true;
      }
   }
   return false;
}
 
/* Function handles a received ARP packet */
static void HandleReceivedArpPacket(struct sr_instance* sr, sr_arp_hdr_t * packet,
   unsigned int length, struct sr_if* interface)
{
   if (length < sizeof(sr_arp_hdr_t))
   {
      return;
   }
   
   if ((ntohs(packet->ar_pro) != ethertype_ip)
      || (ntohs(packet->ar_hrd) != arp_hrd_ethernet)
      || (packet->ar_pln != IP_ADDR_LEN) 
      || (packet->ar_hln != ETHER_ADDR_LEN))
   {
      /* Received unsupported packet */
      return; /* Drop */
   }
   
   switch (ntohs(packet->ar_op))
   {
      case arp_op_request:
      {
         if (packet->ar_tip == interface->ip)
         {
            /* We're being ARPed! Prepare the reply! */
            uint8_t* replyPacket = (uint8_t *) malloc(sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t));
            sr_ethernet_hdr_t* ethernetHdr = (sr_ethernet_hdr_t*)replyPacket;
            sr_arp_hdr_t* arpHdr = (sr_arp_hdr_t*)(replyPacket + sizeof(sr_ethernet_hdr_t));
            
            /* Receievs ARP request. Send ARP reply.*/            
            /* Ethernet Header */
            memcpy(ethernetHdr->ether_dhost, packet->ar_sha, ETHER_ADDR_LEN);
            memcpy(ethernetHdr->ether_shost, interface->addr, ETHER_ADDR_LEN);
            ethernetHdr->ether_type = htons(ethertype_arp);
            
            /* ARP Header */
            arpHdr->ar_hrd = htons(arp_hrd_ethernet);
            arpHdr->ar_pro = htons(ethertype_ip);
            arpHdr->ar_hln = ETHER_ADDR_LEN;
            arpHdr->ar_pln = IP_ADDR_LEN;
            arpHdr->ar_op = htons(arp_op_reply);
            memcpy(arpHdr->ar_sha, interface->addr, ETHER_ADDR_LEN);
            arpHdr->ar_sip = interface->ip;
            memcpy(arpHdr->ar_tha, packet->ar_sha, ETHER_ADDR_LEN);
            arpHdr->ar_tip = packet->ar_sip;
            
            sr_send_packet(sr, replyPacket, sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t),
               interface->name);
            
            free(replyPacket);
         }
         break;
      }
      
      case arp_op_reply:
      {
         if (packet->ar_tip == interface->ip)
         {
            struct sr_arpreq* requestARPPointer = sr_arpcache_insert(
               &sr->cache, packet->ar_sha, ntohl(packet->ar_sip));
            
            if (requestARPPointer != NULL)
            {
            /* Receives ARP reply, send all packets in queue */                
               while (requestARPPointer->packets != NULL)
               {
                  struct sr_packet* curr = requestARPPointer->packets;
                  
                  memcpy(((sr_ethernet_hdr_t*) curr->buf)->ether_dhost,
                     packet->ar_sha, ETHER_ADDR_LEN);

                  /* Send packet in queue */
                  sr_send_packet(sr, curr->buf, curr->len, curr->iface);
                  
                  /* Forward list of packets. */
                  requestARPPointer->packets = requestARPPointer->packets->next;
                  
                  /* Free all memory associated with this packet (allocated on queue). */
                  free(curr->buf);
                  free(curr->iface);
                  free(curr);
               }
               
               sr_arpreq_destroy(&sr->cache, requestARPPointer);
            }
            else
            {
            fprintf(stderr, " Receives ARP reply, no found ARP requests ");
            }
         }
         break;
      }
      
      default:
      {
         /* Unrecognized ARP type */
         break;
      }
   }
}
 
/* Function handles a received IPv4 packet*/
static void HandleReceivedIpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet,
   unsigned int length, struct sr_if* interface)
{
   if (length < sizeof(sr_ip_hdr_t))
   {
      return; /* Invalid size. Drop !!!*/
   }
   
   if (packet->ip_hl >= MIN_IP_HEADER_LEN)
   {
      uint16_t headerChecksum = packet->ip_sum;
      uint16_t calculatedChecksum = 0;
      packet->ip_sum = 0;
      
      calculatedChecksum = cksum(packet, getIpHeaderLength(packet));
      
      if (headerChecksum != calculatedChecksum)
      {
         return; /* Checksum fail. Drop !!! */
      }
      else
      {
         packet->ip_sum = headerChecksum; /* Put checksum back ! */
      }
   }
   else
   {
      return; /* Invalid length. Drop ! */
   }
   
   if (packet->ip_v != IP_VERSION)
   {
      /* Process IPv4 packets only.*/
      return; /* Not IPv4. Drop !*/
   }
   
   if (!natEnabled(sr))
   {
      if (IpDestinationIsRouter(sr, packet))
      {
         HandleReceivedPacketToRouter(sr, packet, length, interface);
      }
      else
      {
         ForwardIpPacket(sr, packet, length, interface);
      }
   }
   else
   {
      natHandleRecievedIpPacket(sr, packet, length, interface);
   }
}
 
/* Function handles a received ICMP packet. */
static void HandleIcmpPacket(struct sr_instance* sr, sr_ip_hdr_t* packet,
   unsigned int length, struct sr_if* interface)
{
   sr_icmp_hdr_t* icmpHeader = (sr_icmp_hdr_t*) (((uint8_t*) packet) + getIpHeaderLength(packet));
   int icmpLength = length - getIpHeaderLength(packet);
   
   if (!IcmpPerformIntegrityCheck(icmpHeader, icmpLength))
   {
      return; /* Drop.*/
   }
   
   if (icmpHeader->icmp_type == icmp_type_echo_request)
   {
      /* Send an echo Reply! */
      SendIcmpEchoReply(sr, packet, length);
   }
   else
   {
      /* Receive unexpected ICMP packet.*/
   }
}

/* Function handles send ICMP Echo reply */
static void SendIcmpEchoReply(struct sr_instance* sr, sr_ip_hdr_t* echoRequestPacket,
   unsigned int length)
{
   int icmpLength = length - getIpHeaderLength(echoRequestPacket);
   uint8_t* replyPacket = malloc(icmpLength + sizeof(sr_ip_hdr_t) + sizeof(sr_ethernet_hdr_t));
   sr_ip_hdr_t* replyIpHeader = (sr_ip_hdr_t*) (replyPacket + sizeof(sr_ethernet_hdr_t));
   sr_icmp_hdr_t* replyIcmpHeader =
      (sr_icmp_hdr_t*) ((uint8_t*) replyIpHeader + sizeof(sr_ip_hdr_t));
   assert(replyPacket);
      
   /* IP Header */
   replyIpHeader->ip_v = IP_VERSION;
   replyIpHeader->ip_hl = MIN_IP_HEADER_LEN;
   replyIpHeader->ip_tos = 0;
   replyIpHeader->ip_len = htons((uint16_t) length);
   replyIpHeader->ip_id = htons(ipIdentifyNumber);
   ipIdentifyNumber++;
   replyIpHeader->ip_off = htons(IP_DF);
   replyIpHeader->ip_ttl = DEFAULT_TTL;
   replyIpHeader->ip_p = ip_protocol_icmp;
   replyIpHeader->ip_sum = 0;
   replyIpHeader->ip_src = echoRequestPacket->ip_dst; /* Already in network byte order */
   replyIpHeader->ip_dst = echoRequestPacket->ip_src; /* Already in network byte order */
   replyIpHeader->ip_sum = cksum(replyIpHeader, getIpHeaderLength(replyIpHeader));
   
   /* ICMP header */
   replyIcmpHeader->icmp_type = icmp_type_echo_reply;
   replyIcmpHeader->icmp_code = 0;
   replyIcmpHeader->icmp_sum = 0;
   
   /* Copy the old payload into the new packet */
   memcpy(((uint8_t*) replyIcmpHeader) + sizeof(sr_icmp_hdr_t),
      ((uint8_t*) echoRequestPacket) + getIpHeaderLength(echoRequestPacket) + sizeof(sr_icmp_hdr_t), 
      icmpLength - sizeof(sr_icmp_hdr_t));
   
   /* Then update the final checksum for the ICMP payload */
   replyIcmpHeader->icmp_sum = cksum(replyIcmpHeader, icmpLength);
   
   /* Send ICMP packet. */
   handleArpAndSendPacket(sr, (sr_ethernet_hdr_t*) replyPacket, length + sizeof(sr_ethernet_hdr_t),
      getLongestPrefixRoute(sr, ntohl(echoRequestPacket->ip_src)));
   
   free(replyPacket);
}
 
/* Function handles send ICMP Time Exceed (NAT may be enable)*/
static void SendIcmpTimeExceed(struct sr_instance* sr, sr_ip_hdr_t* originalPacket,
   unsigned int length, sr_if_t *receivedInterface)
{
   uint8_t* replyPacket = malloc(sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + 
                                                         sizeof(sr_icmp_t3_hdr_t));
   sr_ip_hdr_t* replyIpHeader = (sr_ip_hdr_t*) (replyPacket + sizeof(sr_ethernet_hdr_t));
   sr_icmp_t3_hdr_t* replyIcmpHeader = (sr_icmp_t3_hdr_t*) ((uint8_t*) replyIpHeader
      + sizeof(sr_ip_hdr_t));
   
   if (natEnabled(sr))
   {
      natNotdonePacketMapping(sr, originalPacket, length, receivedInterface);
   }

   /* IP header. */
   replyIpHeader->ip_v = IP_VERSION;
   replyIpHeader->ip_hl = MIN_IP_HEADER_LEN;
   replyIpHeader->ip_tos = 0;
   replyIpHeader->ip_len = htons(sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
   replyIpHeader->ip_id = htons(ipIdentifyNumber);
   ipIdentifyNumber++;
   replyIpHeader->ip_off = htons(IP_DF);
   replyIpHeader->ip_ttl = DEFAULT_TTL;
   replyIpHeader->ip_p = ip_protocol_icmp;
   replyIpHeader->ip_sum = 0;
   replyIpHeader->ip_src = receivedInterface->ip;
   replyIpHeader->ip_dst = originalPacket->ip_src; /* Already in network byte order. */
   replyIpHeader->ip_sum = cksum(replyIpHeader, getIpHeaderLength(replyIpHeader));
   
   /*  ICMP header. */
   replyIcmpHeader->icmp_type = icmp_type_time_exceeded;
   replyIcmpHeader->icmp_code = 0;
   replyIcmpHeader->icmp_sum = 0;
   memcpy(replyIcmpHeader->data, originalPacket, ICMP_DATA_SIZE);
   replyIcmpHeader->icmp_sum = cksum(replyIcmpHeader, sizeof(sr_icmp_t3_hdr_t));
   
   /* Send ICMP Time Exceed. */
   handleArpAndSendPacket(sr, (sr_ethernet_hdr_t*) replyPacket,
      sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t),
      getLongestPrefixRoute(sr, ntohl(originalPacket->ip_src)));
   
   free(replyPacket);
}

/* Function handles send packet immediately if find in cache. Or send ARP request to
find ip->mac mapping.*/
static void handleArpAndSendPacket(sr_instance_t *sr, sr_ethernet_hdr_t* packet, 
   unsigned int length, sr_rt_t* route)
{
   uint32_t nextHopIpAddress;
   sr_arpentry_t *arpEntry;
      
   nextHopIpAddress = ntohl(route->gw.s_addr);
   arpEntry = sr_arpcache_lookup(&sr->cache, nextHopIpAddress);
   
   packet->ether_type = htons(ethertype_ip);
   memcpy(packet->ether_shost, sr_get_interface(sr, route->interface)->addr, ETHER_ADDR_LEN);
   
   if (arpEntry != NULL)
   {
      memcpy(packet->ether_dhost, arpEntry->mac, ETHER_ADDR_LEN);
      sr_send_packet(sr, (uint8_t*) packet, length, route->interface);
      
      free(arpEntry); /* Make a copy then free to prevent memory leak.*/
   }
   else
   {
      /* Setup the request and send the ARP packet. */
      struct sr_arpreq* arpRequest = sr_arpcache_queuereq(&sr->cache, nextHopIpAddress,
         (uint8_t*) packet, length, route->interface);
      
      if (arpRequest->times_sent == 0)
      {
         /* New request. Then send the first */
         arpRequest->requestedInterface = sr_get_interface(sr, route->interface);
         
         SendArpRequest(sr, arpRequest);
         
         /* Update other fields */
         arpRequest->times_sent = 1;
         arpRequest->sent = time(NULL);
      }
   }
}
 
/* Function checks if IP addresses matches source IP.*/
static bool IpSourceForRouter(struct sr_instance* sr, sr_ip_hdr_t* packet)
{
   struct sr_if* interfaceWalker;
   
   for (interfaceWalker = sr->if_list; interfaceWalker != NULL; interfaceWalker =
      interfaceWalker->next)
   {
      if (packet->ip_src == interfaceWalker->ip)
      {
         return true;
      }
   }  
   return false;
}

/* Function gets the length of a provided IPv4 subnet mask.*/
static int getLongestPrefixLength(uint32_t mask)
{
  int ret = 0;
  uint32_t move = 0x80000000;
  
  while ((move != 0) && ((move & mask) != 0))
  {
    move >>= 1;
    ret++;
  }
  return ret;
}