/*
    ettercap -- initial scan to build the hosts list

    Copyright (C) ALoR & NaGA

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

    $Header: /home/drizzt/dev/sources/ettercap.cvs/ettercap_ng/src/ec_scan.c,v 1.9 2003/05/26 20:02:14 alor Exp $
*/

#include <ec.h>
#include <ec_packet.h>
#include <ec_threads.h>
#include <ec_send.h>
#include <ec_decode.h>
#include <ec_resolv.h>

#include <pthread.h>
#include <pcap.h>
#include <libnet.h>

/* globals */

/* used to create the random list */
SLIST_HEAD (, ip_list) ip_list_head;
struct ip_list **rand_array;

/* protos */

void build_hosts_list(void);
void scan_netmask(void);
void scan_targets(void);

void load_hosts(char *filename);
void save_hosts(char *filename);

void add_host(struct ip_addr *ip, u_int8 mac[ETH_ADDR_LEN], char *name);

void random_list(struct ip_list *e, int max);

void get_response(struct packet_object *po);
EC_THREAD_FUNC(capture_scan);
void scan_decode(u_char *param, const struct pcap_pkthdr *pkthdr, const u_char *pkt);

/*******************************************/

/*
 * build the initial host list with ARP requests
 */

void build_hosts_list(void)
{
   pthread_t pid;
   struct hosts_list *hl;
   int nhosts = 0, i = 1;
   //char tmp[MAX_ASCII_ADDR_LEN];

   DEBUG_MSG("build_hosts_list");

   /* 
    * load the list from the file 
    * this option automatically enable GBL_OPTIONS->silent
    */
   if (GBL_OPTIONS->load_hosts) {
      load_hosts(GBL_OPTIONS->hostsfile);
      
      LIST_FOREACH(hl, &GBL_HOSTLIST, next)
         nhosts++;

      USER_MSG("%d hosts added to the hosts list...\n", nhosts);
      ui_msg_flush(1);
   }
   
   /* in silent mode, the list should not be created */
   if (GBL_OPTIONS->silent)
      return;
      
   /* even in offline sniffing the list should not be created */
   if (GBL_OPTIONS->read)
      return;
   
   /*
    * create a simple decode thread, it will call
    * the right HOOK POINT. so we only have to hook to 
    * ARP packets.
    */

   hook_add(PACKET_ARP_RP, &get_response);
   pid = ec_thread_new("scan_cap", "decoder module while scanning", &capture_scan, NULL);
  
   /* 
    * no target were specified, we have to make a list
    * scanning the whole netmask
    */
   if (GBL_TARGET1->all_ip && GBL_TARGET2->all_ip)
      scan_netmask();
   else
      scan_targets();

   /* 
    * free the temporary array for random computations 
    * allocated in rando_list()
    */
   SAFE_FREE(rand_array);
   
   /* 
    * wait for some delayed packets... 
    * the other thread is listening for ARP pachets
    */
   sleep(1);

   /* destroy the thread and remobe the hook function */
   ec_thread_destroy(pid);
   hook_del(PACKET_ARP, &get_response);
   
   /* count the hosts and print the message */
   LIST_FOREACH(hl, &GBL_HOSTLIST, next) {
      char tmp[MAX_ASCII_ADDR_LEN];
      (void)tmp;
      DEBUG_MSG("Host: %s", ip_addr_ntoa(&hl->ip, tmp));
      nhosts++;
   }

   USER_MSG("%d hosts added to the hosts list...\n", nhosts);
   ui_msg_flush(1);
  
   /* 
    * resolve the hostnames only if we are scanning 
    * the lan. when loading from file, hostnames are
    * already in the file.
    */
   
   if (!GBL_OPTIONS->load_hosts && GBL_OPTIONS->resolve) {
      
      USER_MSG("Resolving %d hostnames...\n", nhosts);
      ui_msg_flush(1);
      
      LIST_FOREACH(hl, &GBL_HOSTLIST, next) {
         char tmp[MAX_HOSTNAME_LEN];
         
         host_iptoa(&hl->ip, tmp);
         hl->hostname = strdup(tmp);
         
         ui_progress(i++, nhosts);
      }
   }
   
   /* save the list to the file */
   if (GBL_OPTIONS->save_hosts)
      save_hosts(GBL_OPTIONS->hostsfile);

}


/*
 * capture the packets and call the HOOK POINT
 */

EC_THREAD_FUNC(capture_scan)
{
   DEBUG_MSG("capture_scan");

   ec_thread_init();

   pcap_loop(GBL_PCAP->pcap, -1, scan_decode, EC_THREAD_PARAM);

   return NULL;
}

/* 
 * parses the POs and executes the HOOK POINTs
 */

void scan_decode(u_char *param, const struct pcap_pkthdr *pkthdr, const u_char *pkt)
{
   struct packet_object *po;
   int len;
   u_char *data;
   int datalen;
   
   CANCELLATION_POINT();

   /* extract data and datalen from pcap packet */
   data = (u_char *)pkt;
   datalen = pkthdr->caplen;
   
   /* alloc the packet object structure to be passet through decoders */
   packet_create_object(&po, data, datalen);

   /* set the po timestamp */
   memcpy(&po->ts, &pkthdr->ts, sizeof(struct timeval));
   
   /* 
    * in this special parsing, the packet must be ingored by
    * application layer, leave this un touched.
    */
   po->flags |= PO_IGNORE;
  
   /* 
    * start the analysis through the decoders stack 
    * after this fuction the packet is completed (all flags set)
    */
   l2_decoder(data, datalen, &len, po);
   
   /* free the structure */
   packet_destroy_object(&po);
   
   /* clean the buffer */
   memset((u_char *)pkt, 0, pkthdr->caplen);
   
   CANCELLATION_POINT();

   return;
}

/*
 * receives the ARP packets
 */

void get_response(struct packet_object *po)
{
   add_host(&po->L3.src, po->L2.src, NULL);
}


/* 
 * scan the netmask to find all hosts 
 */
void scan_netmask(void)
{
   u_int32 netmask, current, myip;
   int nhosts, i;
   struct ip_addr scanip;
   struct ip_list *e; 

   netmask = *(u_int32 *)&GBL_IFACE->netmask.addr;
   myip = *(u_int32 *)&GBL_IFACE->ip.addr;
   
   /* the number of hosts in this netmask */
   nhosts = ntohl(~netmask);

   DEBUG_MSG("scan_netmask: %d hosts", nhosts);

   USER_MSG("Randomizing %d hosts for scanning...\n", nhosts);
   ui_msg_flush(1);
  
   /* scan the netmask */
   for (i = 1; i <= nhosts; i++){
      /* calculate the ip */
      current = (myip & netmask) | htonl(i);
      ip_addr_init(&scanip, AF_INET, (char *)&current);
      
      e = calloc(1, sizeof(struct ip_list));
      ON_ERROR(e, NULL, "can't allocate memory");
      
      memcpy(&e->ip, &scanip, sizeof(struct ip_addr));
      
      /* add to the list randomly */
      random_list(e, i);
      
      //ui_progress(i, nhosts);
   }

   USER_MSG("Scanning the whole netmask for %d hosts...\n", nhosts);
   ui_msg_flush(1);
   
   i = 1;
   
   /* send the actual ARP request */
   SLIST_FOREACH(e, &ip_list_head, next) {
      /* send the arp request */
      send_arp(ARPOP_REQUEST, &GBL_IFACE->ip, GBL_IFACE->mac, &e->ip, ETH_BROADCAST);

      /* update the progress bar */
      ui_progress(i++, nhosts);
      
      /* wait for a delay */
      usleep(GBL_OPTIONS->scan_delay * 1000);
   }
   
   /* delete the temporary list */
   while (SLIST_FIRST(&ip_list_head) != NULL) {                                                           
      e = SLIST_FIRST(&ip_list_head);                                                                     
      SLIST_REMOVE_HEAD(&ip_list_head, next);                                                             
      SAFE_FREE(e);                                                                                 
   }  
}

/*
 * scan only the target hosts
 */

void scan_targets(void)
{
   int nhosts = 0, found, n = 1;
   struct ip_list *e, *i, *m; 
   
   DEBUG_MSG("scan_targets: merging targets...");

   /* 
    * make an unique list merging the two target
    * and count the number of hosts to be scanned
    */
   
   /* first get all the target1 ips */
   SLIST_FOREACH(i, &GBL_TARGET1->ips, next) {
      
      e = calloc(1, sizeof(struct ip_list));
      ON_ERROR(e, NULL, "can't allocate memory");
      
      memcpy(&e->ip, &i->ip, sizeof(struct ip_addr));
      
      nhosts++;
      
      /* add to the list randomly */
      random_list(e, nhosts);
   }

   /* then merge the target2 ips */
   SLIST_FOREACH(i, &GBL_TARGET2->ips, next) {
      
      e = calloc(1, sizeof(struct ip_list));
      ON_ERROR(e, NULL, "can't allocate memory");
      
      memcpy(&e->ip, &i->ip, sizeof(struct ip_addr));
      
      found = 0;
     
      /* search if it is already in the list */
      SLIST_FOREACH(m, &ip_list_head, next)
         if (!ip_addr_cmp(&m->ip, &i->ip)) {
            found = 1;
            SAFE_FREE(e);
            break;
         }
      
      /* add it */
      if (!found) {
         nhosts++;
         /* add to the list randomly */
         random_list(e, nhosts);
      }
   }
   
   DEBUG_MSG("scan_targets: %d hosts to be scanned", nhosts);

   USER_MSG("Scanning for merged targets (%d hosts)...\n\n", nhosts);
   
   /* print the above message */
   ui_msg_flush(1);
   
   /* and now scan the LAN */

   SLIST_FOREACH(e, &ip_list_head, next) {
      /* send the arp request */
      send_arp(ARPOP_REQUEST, &GBL_IFACE->ip, GBL_IFACE->mac, &e->ip, ETH_BROADCAST);

      /* update the progress bar */
      ui_progress(n++, nhosts);
      
      /* wait for a delay */
      usleep(GBL_OPTIONS->scan_delay * 1000);
   }
  
   /* delete the temporary list */
   while (SLIST_FIRST(&ip_list_head) != NULL) {                                                           
      e = SLIST_FIRST(&ip_list_head);                                                                     
      SLIST_REMOVE_HEAD(&ip_list_head, next);                                                             
      SAFE_FREE(e);                                                                                 
   }  
   
}

/*
 * load the hosts list from this file
 */

void load_hosts(char *filename)
{
   FILE *hf;
   int nhosts;
   char ip[16], mac[18], name[128];
   struct in_addr tip;
   struct ip_addr hip;
   u_int8 hmac[ETH_ADDR_LEN];
   
   DEBUG_MSG("load_hosts: %s", filename);

   /* open the file */
   hf = fopen(filename, "r");
   ON_ERROR(hf, NULL, "Cannot open %s", filename);
 
   USER_MSG("Loading hosts list from file %s\n", filename);
   /* print the above message */
   ui_msg_flush(1);
   
   /* XXX - adapt to IPv6 */
   /* read the file */
   for (nhosts = 0; !feof(hf); nhosts++) {
      fscanf(hf,"%15s %17s %127s\n", ip, mac, name);
      
      /* convert to network */
      mac_addr_aton(mac, hmac);
      
      if (inet_aton(ip, &tip) == 0)
         FATAL_ERROR("Bad parsing on line %d", nhosts);
      
      ip_addr_init(&hip, AF_INET, (char *)&tip);
      
      /* add to the list */
      add_host(&hip, hmac, name);
   }

   fclose(hf);
}

/*
 * save the host list to this file 
 */
void save_hosts(char *filename)
{
   FILE *hf;
   int nhosts = 0;
   struct hosts_list *hl;
   char tmp[MAX_ASCII_ADDR_LEN];
   
   DEBUG_MSG("save_hosts: %s", filename);
   
   /* open the file */
   hf = fopen(filename, "w");
   ON_ERROR(hf, NULL, "Cannot open %s for writing", filename);
 
   /* save the list */
   LIST_FOREACH(hl, &GBL_HOSTLIST, next) {
      fprintf(hf, "%s ", ip_addr_ntoa(&hl->ip, tmp));
      fprintf(hf, "%s %s\n", mac_addr_ntoa(hl->mac, tmp), hl->hostname);
      nhosts++;
   }
  
   /* close the file */
   fclose(hf);
   
   USER_MSG("%d hosts saved to file %s\n", nhosts, filename);
   /* print the above message */
   ui_msg_flush(1);
   
}

/*
 * add an host to the list 
 * order the list while inserting the elements
 */

void add_host(struct ip_addr *ip, u_int8 mac[ETH_ADDR_LEN], char *name)
{
   struct hosts_list *hl, *h;

   h = calloc(1, sizeof(struct hosts_list));
   ON_ERROR(h, NULL, "can't allocate memory");

   /* fill the struct */
   memcpy(&h->ip, ip, sizeof(struct ip_addr));
   memcpy(&h->mac, mac, ETH_ADDR_LEN);

   if (name)
      h->hostname = strdup(name);
   
   /* insert in order (ascending) */
   LIST_FOREACH(hl, &GBL_HOSTLIST, next) {
      
      if (ip_addr_cmp(&hl->ip, &h->ip) < 0 && LIST_NEXT(hl, next) != LIST_END(&GBL_HOSTLIST) )
         continue;
      else if (ip_addr_cmp(&h->ip, &hl->ip) > 0) {
         LIST_INSERT_AFTER(hl, h, next);  
         break;
      } else {
         LIST_INSERT_BEFORE(hl, h, next);
         break;
      }
         
   }

   /* the first element */
   if (LIST_FIRST(&GBL_HOSTLIST) == LIST_END(&GBL_HOSTLIST))
      LIST_INSERT_HEAD(&GBL_HOSTLIST, h, next);
   
}


/*
 * insert the element in the list randomly.
 * 'max' is the number of elements in the list
 */

void random_list(struct ip_list *e, int max)
{
   int rnd;
   
   srand(time(NULL));

   /* calculate the position in the list. */
   rnd = rand() % ((max == 1) ? max : max - 1);
   
   //rnd = 1+(int) ((float)max*rand()/(RAND_MAX+1.0));

   /* allocate the array used to keep track of the pointer
    * to the elements in the list. this array speed up the
    * access method to the list 
    */
   rand_array = realloc(rand_array, (max + 1) * sizeof(struct ip_addr *));
   ON_ERROR(rand_array, NULL, "realloc(): rand_array");
   
   /* the first element */
   if (SLIST_FIRST(&ip_list_head) == SLIST_END(&ip_list_head)) {
      SLIST_INSERT_HEAD(&ip_list_head, e, next);
      rand_array[0] = e;
      return;
   }

   /* bound checking */
   rnd = (rnd > 1) ? rnd : 1;
   
   /* insert the element in the list */
   SLIST_INSERT_AFTER(rand_array[rnd - 1], e, next);
   /* and add the pointer in the array */
   rand_array[max - 1] = e;
   
}

/* EOF */

// vim:ts=3:expandtab

