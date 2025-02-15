/*
 * Back-end of the driver for virtual network devices. This portion of the
 * driver exports a 'unified' network-device interface that can be accessed
 * by any operating system that implements a compatible front end. A
 * reference front-end implementation can be found in:
 *  drivers/net/xen-netfront.c
 *
 * Copyright (c) 2002-2005, K A Fraser
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "common.h"

#include <linux/kthread.h>
#include <linux/if_vlan.h>
#include <linux/udp.h>

#include <net/tcp.h>

#include <xen/events.h>
#include <xen/interface/memory.h>

#include <asm/xen/hypercall.h>
#include <asm/xen/page.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/etherdevice.h>


#define NEW_NETBACK


struct pending_tx_info {
	struct xen_netif_tx_request req;
	struct xenvif *vif;
};
typedef unsigned int pending_ring_idx_t;

struct netbk_rx_meta {
	int id;
	int size;
	int gso_size;
};

#define MAX_PENDING_REQS 256

/* Discriminate from any valid pending_idx value. */
#define INVALID_PENDING_IDX 0xFFFF

#define MAX_BUFFER_OFFSET PAGE_SIZE

/* extra field used in struct page */
union page_ext {
	struct {
#if BITS_PER_LONG < 64
#define IDX_WIDTH   8
#define GROUP_WIDTH (BITS_PER_LONG - IDX_WIDTH)
		unsigned int group:GROUP_WIDTH;
		unsigned int idx:IDX_WIDTH;
#else
		unsigned int group, idx;
#endif
	} e;
	void *mapping;
};



struct xen_netbk {
/*RTCA*/
	int priority; //add priority attr to netback device
	wait_queue_head_t wq;
	wait_queue_head_t tx_wq;
	struct task_struct *task;

	struct xenvif *vif;

	struct sk_buff_head rx_queue;
	struct sk_buff_head rx_queue_backup;
	struct sk_buff_head tx_queue;
	struct sk_buff_head tx_queue_backup;

	int gso_flag;
	struct sk_buff *gso_skb;

	struct timer_list net_timer;

	struct page *mmap_pages[MAX_PENDING_REQS];

	pending_ring_idx_t pending_prod;
	pending_ring_idx_t pending_cons;
	struct list_head net_schedule_list;

	/* Protect the net_schedule_list in netif. */
	spinlock_t net_schedule_list_lock;

	atomic_t netfront_count;

	struct pending_tx_info pending_tx_info[MAX_PENDING_REQS];
	struct gnttab_copy tx_copy_ops[MAX_PENDING_REQS];

	u16 pending_ring[MAX_PENDING_REQS];

	/*
	 * Given MAX_BUFFER_OFFSET of 4096 the worst case is that each
	 * head/fragment page uses 2 copy operations because it
	 * straddles two buffers in the frontend.
	 */
	struct gnttab_copy grant_copy_op[2*XEN_NETIF_RX_RING_SIZE];
	struct netbk_rx_meta meta[2*XEN_NETIF_RX_RING_SIZE];
};

static struct xen_netbk *xen_netbk;
static int xen_netbk_group_nr;

void xen_netbk_add_xenvif(struct xenvif *vif)
{
	int i;
	int min_netfront_count;
	int min_group = 0;
	struct xen_netbk *netbk;

	min_netfront_count = atomic_read(&xen_netbk[0].netfront_count);
	for (i = 0; i < xen_netbk_group_nr; i++) {
		int netfront_count = atomic_read(&xen_netbk[i].netfront_count);
		if (netfront_count < min_netfront_count) {
			min_group = i;
			min_netfront_count = netfront_count;
		}
	}

	netbk = &xen_netbk[min_group];

/*RTCA*/
#ifdef NEW_NETBACK
	/*if(vif->domid>5){
		netbk=&xen_netbk[5];
		netbk->priority=5;
	}

	else{
		netbk=&xen_netbk[vif->domid-1];
		netbk->priority=vif->domid-1;
		if(vif->domid==2 ||vif->domid==3||vif->domid==4||vif->domid==5){
			netbk=&xen_netbk[1];
			netbk->priority=1;
		}
	}
	//if(vif->domid==1||vif->domid==2)
		netbk->vif=NULL;*/

	if(vif->priority >5){
		netbk=&xen_netbk[5];
	} else{
		netbk=&xen_netbk[vif->priority];
	}
	netbk->vif=NULL;
#endif
	vif->netbk = netbk;
	
	atomic_inc(&netbk->netfront_count);
}

void xen_netbk_remove_xenvif(struct xenvif *vif)
{
	struct xen_netbk *netbk = vif->netbk;
	vif->netbk = NULL;
	atomic_dec(&netbk->netfront_count);
}

static void xen_netbk_idx_release(struct xen_netbk *netbk, u16 pending_idx);
static void make_tx_response(struct xenvif *vif,
			     struct xen_netif_tx_request *txp,
			     s8       st);
static struct xen_netif_rx_response *make_rx_response(struct xenvif *vif,
					     u16      id,
					     s8       st,
					     u16      offset,
					     u16      size,
					     u16      flags);

static inline unsigned long idx_to_pfn(struct xen_netbk *netbk,
				       u16 idx)
{
	return page_to_pfn(netbk->mmap_pages[idx]);
}

static inline unsigned long idx_to_kaddr(struct xen_netbk *netbk,
					 u16 idx)
{
	return (unsigned long)pfn_to_kaddr(idx_to_pfn(netbk, idx));
}

/* extra field used in struct page */
static inline void set_page_ext(struct page *pg, struct xen_netbk *netbk,
				unsigned int idx)
{
	unsigned int group = netbk - xen_netbk;
	union page_ext ext = { .e = { .group = group + 1, .idx = idx } };

	BUILD_BUG_ON(sizeof(ext) > sizeof(ext.mapping));
	pg->mapping = ext.mapping;
}

static int get_page_ext(struct page *pg,
			unsigned int *pgroup, unsigned int *pidx)
{
	union page_ext ext = { .mapping = pg->mapping };
	struct xen_netbk *netbk;
	unsigned int group, idx;

	group = ext.e.group - 1;

	if (group < 0 || group >= xen_netbk_group_nr)
		return 0;

	netbk = &xen_netbk[group];

	idx = ext.e.idx;

	if ((idx < 0) || (idx >= MAX_PENDING_REQS))
		return 0;

	if (netbk->mmap_pages[idx] != pg)
		return 0;

	*pgroup = group;
	*pidx = idx;

	return 1;
}

/*
 * This is the amount of packet we copy rather than map, so that the
 * guest can't fiddle with the contents of the headers while we do
 * packet processing on them (netfilter, routing, etc).
 */
#define PKT_PROT_LEN    (ETH_HLEN + \
			 VLAN_HLEN + \
			 sizeof(struct iphdr) + MAX_IPOPTLEN + \
			 sizeof(struct tcphdr) + MAX_TCP_OPTION_SPACE)

static u16 frag_get_pending_idx(skb_frag_t *frag)
{
	return (u16)frag->page_offset;
}

static void frag_set_pending_idx(skb_frag_t *frag, u16 pending_idx)
{
	frag->page_offset = pending_idx;
}

static inline pending_ring_idx_t pending_index(unsigned i)
{
	return i & (MAX_PENDING_REQS-1);
}

static inline pending_ring_idx_t nr_pending_reqs(struct xen_netbk *netbk)
{
	return MAX_PENDING_REQS -
		netbk->pending_prod + netbk->pending_cons;
}

static void xen_netbk_kick_thread(struct xen_netbk *netbk)
{
	//wake_up(&netbk->wq);
	if(!list_empty(&((netbk->wq).task_list))){
		wake_up(&netbk->wq);
	}
	/*if(!list_empty(&((netbk->tx_wq).task_list))){
		wake_up(&netbk->tx_wq);
	}*/
}

static int max_required_rx_slots(struct xenvif *vif)
{
	int max = DIV_ROUND_UP(vif->dev->mtu, PAGE_SIZE);

	if (vif->can_sg || vif->gso || vif->gso_prefix)
		max += MAX_SKB_FRAGS + 1; /* extra_info + frags */

	return max;
}

int xen_netbk_rx_ring_full(struct xenvif *vif)
{
	RING_IDX peek   = vif->rx_req_cons_peek;
	RING_IDX needed = max_required_rx_slots(vif);

	return ((vif->rx.sring->req_prod - peek) < needed) ||
	       ((vif->rx.rsp_prod_pvt + XEN_NETIF_RX_RING_SIZE - peek) < needed);
}

int xen_netbk_must_stop_queue(struct xenvif *vif)
{
	if (!xen_netbk_rx_ring_full(vif))
		return 0;

	vif->rx.sring->req_event = vif->rx_req_cons_peek +
		max_required_rx_slots(vif);
	mb(); /* request notification /then/ check the queue */

	return xen_netbk_rx_ring_full(vif);
}

/*
 * Returns true if we should start a new receive buffer instead of
 * adding 'size' bytes to a buffer which currently contains 'offset'
 * bytes.
 */
static bool start_new_rx_buffer(int offset, unsigned long size, int head)
{
	/* simple case: we have completely filled the current buffer. */
	if (offset == MAX_BUFFER_OFFSET)
		return true;

	/*
	 * complex case: start a fresh buffer if the current frag
	 * would overflow the current buffer but only if:
	 *     (i)   this frag would fit completely in the next buffer
	 * and (ii)  there is already some data in the current buffer
	 * and (iii) this is not the head buffer.
	 *
	 * Where:
	 * - (i) stops us splitting a frag into two copies
	 *   unless the frag is too large for a single buffer.
	 * - (ii) stops us from leaving a buffer pointlessly empty.
	 * - (iii) stops us leaving the first buffer
	 *   empty. Strictly speaking this is already covered
	 *   by (ii) but is explicitly checked because
	 *   netfront relies on the first buffer being
	 *   non-empty and can crash otherwise.
	 *
	 * This means we will effectively linearise small
	 * frags but do not needlessly split large buffers
	 * into multiple copies tend to give large frags their
	 * own buffers as before.
	 */
	if ((offset + size > MAX_BUFFER_OFFSET) &&
	    (size <= MAX_BUFFER_OFFSET) && offset && !head)
		return true;

	return false;
}

/*
 * Figure out how many ring slots we're going to need to send @skb to
 * the guest. This function is essentially a dry run of
 * netbk_gop_frag_copy.
 */
/*unsigned int xen_netbk_count_skb_slots(struct xenvif *vif, struct sk_buff *skb)
{
	unsigned int count;
	int i, copy_off;

	count = DIV_ROUND_UP(
			offset_in_page(skb->data)+skb_headlen(skb), PAGE_SIZE);

	copy_off = skb_headlen(skb) % PAGE_SIZE;

	if (skb_shinfo(skb)->gso_size)
		count++;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		unsigned long size = skb_frag_size(&skb_shinfo(skb)->frags[i]);
		unsigned long bytes;
		while (size > 0) {
			BUG_ON(copy_off > MAX_BUFFER_OFFSET);

			if (start_new_rx_buffer(copy_off, size, 0)) {
				count++;
				copy_off = 0;
			}

			bytes = size;
			if (copy_off + bytes > MAX_BUFFER_OFFSET)
				bytes = MAX_BUFFER_OFFSET - copy_off;

			copy_off += bytes;
			size -= bytes;
		}
	}
	return count;
}*/
	
struct xenvif_count_slot_state {
	unsigned long copy_off;
	bool head;
};

unsigned int xenvif_count_frag_slots(struct xenvif *vif,
				     unsigned long offset, unsigned long size,
				     struct xenvif_count_slot_state *state)
{
	unsigned count = 0;

	offset &= ~PAGE_MASK;

	while (size > 0) {
		unsigned long bytes;

		bytes = PAGE_SIZE - offset;

		if (bytes > size)
			bytes = size;

		if (start_new_rx_buffer(state->copy_off, bytes, state->head)) {
			count++;
			state->copy_off = 0;
		}

		if (state->copy_off + bytes > MAX_BUFFER_OFFSET)
			bytes = MAX_BUFFER_OFFSET - state->copy_off;

		state->copy_off += bytes;

		offset += bytes;
		size -= bytes;

		if (offset == PAGE_SIZE)
			offset = 0;

		state->head = false;
	}

	return count;
}

unsigned int xen_netbk_count_skb_slots(struct xenvif *vif, struct sk_buff *skb)
{
	struct xenvif_count_slot_state state;
	unsigned int count;
	unsigned char *data;
	unsigned i;

	state.head = true;
	state.copy_off = 0;

	/* Slot for the first (partial) page of data. */
	count = 1;

	/* Need a slot for the GSO prefix for GSO extra data? */
	if (skb_shinfo(skb)->gso_size)
		count++;

	data = skb->data;
	while (data < skb_tail_pointer(skb)) {
		unsigned long offset = offset_in_page(data);
		unsigned long size = PAGE_SIZE - offset;

		if (data + size > skb_tail_pointer(skb))
			size = skb_tail_pointer(skb) - data;

		count += xenvif_count_frag_slots(vif, offset, size, &state);

		data += size;
	}

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		unsigned long size = skb_frag_size(&skb_shinfo(skb)->frags[i]);
		unsigned long offset = skb_shinfo(skb)->frags[i].page_offset;

		count += xenvif_count_frag_slots(vif, offset, size, &state);
	}
	return count;
}


struct netrx_pending_operations {
	unsigned copy_prod, copy_cons;
	unsigned meta_prod, meta_cons;
	struct gnttab_copy *copy;
	struct netbk_rx_meta *meta;
	int copy_off;
	grant_ref_t copy_gref;
};

static struct netbk_rx_meta *get_next_rx_buffer(struct xenvif *vif,
						struct netrx_pending_operations *npo)
{
	struct netbk_rx_meta *meta;
	struct xen_netif_rx_request *req;

	req = RING_GET_REQUEST(&vif->rx, vif->rx.req_cons++);

	meta = npo->meta + npo->meta_prod++;
	meta->gso_size = 0;
	meta->size = 0;
	meta->id = req->id;

	npo->copy_off = 0;
	npo->copy_gref = req->gref;

	return meta;
}

/*
 * Set up the grant operations for this fragment. If it's a flipping
 * interface, we also set up the unmap request from here.
 */
static void netbk_gop_frag_copy(struct xenvif *vif, struct sk_buff *skb,
				struct netrx_pending_operations *npo,
				struct page *page, unsigned long size,
				unsigned long offset, int *head)
{
	struct gnttab_copy *copy_gop;
	struct netbk_rx_meta *meta;
	/*
	 * These variables are used iff get_page_ext returns true,
	 * in which case they are guaranteed to be initialized.
	 */
	unsigned int uninitialized_var(group), uninitialized_var(idx);
	int foreign = get_page_ext(page, &group, &idx);
	unsigned long bytes;

	/* Data must not cross a page boundary. */
	BUG_ON(size + offset > PAGE_SIZE<<compound_order(page));

	meta = npo->meta + npo->meta_prod - 1;

	/* Skip unused frames from start of page */
	page += offset >> PAGE_SHIFT;
	offset &= ~PAGE_MASK;

	while (size > 0) {
		BUG_ON(offset >= PAGE_SIZE);
		BUG_ON(npo->copy_off > MAX_BUFFER_OFFSET);

		bytes = PAGE_SIZE - offset;

		if (bytes > size)
			bytes = size;

		if (start_new_rx_buffer(npo->copy_off, size, *head)) {
			/*
			 * Netfront requires there to be some data in the head
			 * buffer.
			 */
			BUG_ON(*head);

			meta = get_next_rx_buffer(vif, npo);
		}

		if (npo->copy_off + bytes > MAX_BUFFER_OFFSET)
			bytes = MAX_BUFFER_OFFSET - npo->copy_off;

		copy_gop = npo->copy + npo->copy_prod++;
		copy_gop->flags = GNTCOPY_dest_gref;
		if (foreign) {
			struct xen_netbk *netbk = &xen_netbk[group];
			struct pending_tx_info *src_pend;

			src_pend = &netbk->pending_tx_info[idx];

			copy_gop->source.domid = src_pend->vif->domid;
			copy_gop->source.u.ref = src_pend->req.gref;
			copy_gop->flags |= GNTCOPY_source_gref;
		} else {
			void *vaddr = page_address(page);
			copy_gop->source.domid = DOMID_SELF;
			copy_gop->source.u.gmfn = virt_to_mfn(vaddr);
		}
		copy_gop->source.offset = offset;
		copy_gop->dest.domid = vif->domid;

		copy_gop->dest.offset = npo->copy_off;
		copy_gop->dest.u.ref = npo->copy_gref;
		copy_gop->len = bytes;

		npo->copy_off += bytes;
		meta->size += bytes;

		offset += bytes;
		size -= bytes;

		/* Next frame */
		if (offset == PAGE_SIZE && size) {
			BUG_ON(!PageCompound(page));
			page++;
			offset = 0;
		}

		/* Leave a gap for the GSO descriptor. */
		if (*head && skb_shinfo(skb)->gso_size && !vif->gso_prefix)
			vif->rx.req_cons++;

		*head = 0; /* There must be something in this buffer now. */

	}
}

/*
 * Prepare an SKB to be transmitted to the frontend.
 *
 * This function is responsible for allocating grant operations, meta
 * structures, etc.
 *
 * It returns the number of meta structures consumed. The number of
 * ring slots used is always equal to the number of meta slots used
 * plus the number of GSO descriptors used. Currently, we use either
 * zero GSO descriptors (for non-GSO packets) or one descriptor (for
 * frontend-side LRO).
 */
static int netbk_gop_skb(struct sk_buff *skb,
			 struct netrx_pending_operations *npo)
{
	struct xenvif *vif = netdev_priv(skb->dev);
	int nr_frags = skb_shinfo(skb)->nr_frags;
	int i;
	struct xen_netif_rx_request *req;
	struct netbk_rx_meta *meta;
	unsigned char *data;
	int head = 1;
	int old_meta_prod;

	old_meta_prod = npo->meta_prod;

	/* Set up a GSO prefix descriptor, if necessary */
	if (skb_shinfo(skb)->gso_size && vif->gso_prefix) {
		req = RING_GET_REQUEST(&vif->rx, vif->rx.req_cons++);
		meta = npo->meta + npo->meta_prod++;
		meta->gso_size = skb_shinfo(skb)->gso_size;
		meta->size = 0;
		meta->id = req->id;
	}

	req = RING_GET_REQUEST(&vif->rx, vif->rx.req_cons++);
	meta = npo->meta + npo->meta_prod++;

	if (!vif->gso_prefix)
		meta->gso_size = skb_shinfo(skb)->gso_size;
	else
		meta->gso_size = 0;

	meta->size = 0;
	meta->id = req->id;
	npo->copy_off = 0;
	npo->copy_gref = req->gref;

	data = skb->data;
	while (data < skb_tail_pointer(skb)) {
		unsigned int offset = offset_in_page(data);
		unsigned int len = PAGE_SIZE - offset;

		if (data + len > skb_tail_pointer(skb))
			len = skb_tail_pointer(skb) - data;

		//printk("skb head len=%d, offset=%d\n", len, offset);

		netbk_gop_frag_copy(vif, skb, npo,
				    virt_to_page(data), len, offset, &head);
		data += len;
	}

	for (i = 0; i < nr_frags; i++) {
		//printk("skb frags len=%d, offset=%d\n", skb_frag_size(&skb_shinfo(skb)->frags[i]), skb_shinfo(skb)->frags[i].page_offset);
		netbk_gop_frag_copy(vif, skb, npo,
				    skb_frag_page(&skb_shinfo(skb)->frags[i]),
				    skb_frag_size(&skb_shinfo(skb)->frags[i]),
				    skb_shinfo(skb)->frags[i].page_offset,
				    &head);
	}

	return npo->meta_prod - old_meta_prod;
}

/*
 * This is a twin to netbk_gop_skb.  Assume that netbk_gop_skb was
 * used to set up the operations on the top of
 * netrx_pending_operations, which have since been done.  Check that
 * they didn't give any errors and advance over them.
 */
static int netbk_check_gop(struct xenvif *vif, int nr_meta_slots,
			   struct netrx_pending_operations *npo)
{
	struct gnttab_copy     *copy_op;
	int status = XEN_NETIF_RSP_OKAY;
	int i;

	for (i = 0; i < nr_meta_slots; i++) {
		copy_op = npo->copy + npo->copy_cons++;
		if (copy_op->status != GNTST_okay) {
			netdev_dbg(vif->dev,
				   "Bad status %d from copy to DOM%d.\n",
				   copy_op->status, vif->domid);
			status = XEN_NETIF_RSP_ERROR;
		}
	}

	return status;
}

static void netbk_add_frag_responses(struct xenvif *vif, int status,
				     struct netbk_rx_meta *meta,
				     int nr_meta_slots)
{
	int i;
	unsigned long offset;

	/* No fragments used */
	if (nr_meta_slots <= 1)
		return;

	nr_meta_slots--;

	for (i = 0; i < nr_meta_slots; i++) {
		int flags;
		if (i == nr_meta_slots - 1)
			flags = 0;
		else
			flags = XEN_NETRXF_more_data;

		offset = 0;
		make_rx_response(vif, meta[i].id, status, offset,
				 meta[i].size, flags);
	}
}

struct skb_cb_overlay {
	int meta_slots_used;
};


static void xen_netbk_rx_action(struct xen_netbk *netbk)
{
	struct xenvif *vif = NULL, *tmp;
	s8 status;
	u16 irq, flags;
	struct xen_netif_rx_response *resp;
	struct sk_buff_head rxq;
	struct sk_buff *skb;
	LIST_HEAD(notify);
	int ret;
	int nr_frags;
	int count;
	unsigned long offset;
	struct skb_cb_overlay *sco;

	struct netrx_pending_operations npo = {
		.copy  = netbk->grant_copy_op,
		.meta  = netbk->meta,
	};

	skb_queue_head_init(&rxq);

	count = 0;


	while ((skb = skb_dequeue(&netbk->rx_queue)) != NULL) {
		vif = netdev_priv(skb->dev);
		nr_frags = skb_shinfo(skb)->nr_frags;

		sco = (struct skb_cb_overlay *)skb->cb;
		sco->meta_slots_used = netbk_gop_skb(skb, &npo);

		count += nr_frags + 1;

		__skb_queue_tail(&rxq, skb);

		/* Filled the batch queue? */
		if (count + MAX_SKB_FRAGS >= XEN_NETIF_RX_RING_SIZE)
			break;
	}

	BUG_ON(npo.meta_prod > ARRAY_SIZE(netbk->meta));

	if (!npo.copy_prod)
		return;

	BUG_ON(npo.copy_prod > ARRAY_SIZE(netbk->grant_copy_op));
	gnttab_batch_copy(netbk->grant_copy_op, npo.copy_prod);

	while ((skb = __skb_dequeue(&rxq)) != NULL) {
		sco = (struct skb_cb_overlay *)skb->cb;

		vif = netdev_priv(skb->dev);

		if (netbk->meta[npo.meta_cons].gso_size && vif->gso_prefix) {
			resp = RING_GET_RESPONSE(&vif->rx,
						vif->rx.rsp_prod_pvt++);

			resp->flags = XEN_NETRXF_gso_prefix | XEN_NETRXF_more_data;

			resp->offset = netbk->meta[npo.meta_cons].gso_size;
			resp->id = netbk->meta[npo.meta_cons].id;
			resp->status = sco->meta_slots_used;

			npo.meta_cons++;
			sco->meta_slots_used--;
		}


		vif->dev->stats.tx_bytes += skb->len;
		vif->dev->stats.tx_packets++;

		status = netbk_check_gop(vif, sco->meta_slots_used, &npo);

		if (sco->meta_slots_used == 1)
			flags = 0;
		else
			flags = XEN_NETRXF_more_data;

		if (skb->ip_summed == CHECKSUM_PARTIAL) /* local packet? */
			flags |= XEN_NETRXF_csum_blank | XEN_NETRXF_data_validated;
		else if (skb->ip_summed == CHECKSUM_UNNECESSARY)
			/* remote but checksummed. */
			flags |= XEN_NETRXF_data_validated;

		offset = 0;
		resp = make_rx_response(vif, netbk->meta[npo.meta_cons].id,
					status, offset,
					netbk->meta[npo.meta_cons].size,
					flags);

		if (netbk->meta[npo.meta_cons].gso_size && !vif->gso_prefix) {
			struct xen_netif_extra_info *gso =
				(struct xen_netif_extra_info *)
				RING_GET_RESPONSE(&vif->rx,
						  vif->rx.rsp_prod_pvt++);

			resp->flags |= XEN_NETRXF_extra_info;

			gso->u.gso.size = netbk->meta[npo.meta_cons].gso_size;
			gso->u.gso.type = XEN_NETIF_GSO_TYPE_TCPV4;
			gso->u.gso.pad = 0;
			gso->u.gso.features = 0;

			gso->type = XEN_NETIF_EXTRA_TYPE_GSO;
			gso->flags = 0;
		}

		netbk_add_frag_responses(vif, status,
					 netbk->meta + npo.meta_cons + 1,
					 sco->meta_slots_used);

		RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&vif->rx, ret);

		xenvif_notify_tx_completion(vif);


		if (ret && list_empty(&vif->notify_list))
			list_add_tail(&vif->notify_list, &notify);
		else
			xenvif_put(vif);
		npo.meta_cons += sco->meta_slots_used;
		dev_kfree_skb(skb);
	}

	list_for_each_entry_safe(vif, tmp, &notify, notify_list) {
		notify_remote_via_irq(vif->irq);
		list_del_init(&vif->notify_list);
		xenvif_put(vif);
	}

	/* More work to do? */
	if (!skb_queue_empty(&netbk->rx_queue) &&
			!timer_pending(&netbk->net_timer))
		xen_netbk_kick_thread(netbk);
}

void xen_netbk_queue_tx_skb(struct xenvif *vif, struct sk_buff *skb)
{
	struct xen_netbk *netbk = vif->netbk;

	skb_queue_tail(&netbk->rx_queue, skb);

	xen_netbk_kick_thread(netbk);
}

void kick_rx_backup(struct xenvif *vif){

	xen_netbk_kick_thread(vif->netbk);
}
static void xen_netbk_alarm(unsigned long data)
{
	struct xen_netbk *netbk = (struct xen_netbk *)data;
	xen_netbk_kick_thread(netbk);
}

static int __on_net_schedule_list(struct xenvif *vif)
{
	return !list_empty(&vif->schedule_list);
}

/* Must be called with net_schedule_list_lock held */
static void remove_from_net_schedule_list(struct xenvif *vif)
{
	if (likely(__on_net_schedule_list(vif))) {
		list_del_init(&vif->schedule_list);
		xenvif_put(vif);
	}
}

static struct xenvif *poll_net_schedule_list(struct xen_netbk *netbk)
{
	struct xenvif *vif = NULL;

	spin_lock_irq(&netbk->net_schedule_list_lock);
	if (list_empty(&netbk->net_schedule_list))
		goto out;

	vif = list_first_entry(&netbk->net_schedule_list,
			       struct xenvif, schedule_list);
	if (!vif)
		goto out;

	xenvif_get(vif);

	remove_from_net_schedule_list(vif);
out:
	spin_unlock_irq(&netbk->net_schedule_list_lock);
	return vif;
}

void xen_netbk_schedule_xenvif(struct xenvif *vif)
{
	unsigned long flags;
	struct xen_netbk *netbk = vif->netbk;

	if (__on_net_schedule_list(vif))
		goto kick;

	spin_lock_irqsave(&netbk->net_schedule_list_lock, flags);
	if (!__on_net_schedule_list(vif) &&
	    likely(xenvif_schedulable(vif))) {
		list_add_tail(&vif->schedule_list, &netbk->net_schedule_list);
		xenvif_get(vif);
	}
	spin_unlock_irqrestore(&netbk->net_schedule_list_lock, flags);

kick:
	smp_mb();
	if ((nr_pending_reqs(netbk) < (MAX_PENDING_REQS/2)) &&
	    !list_empty(&netbk->net_schedule_list))
		xen_netbk_kick_thread(netbk);
}



void xen_netbk_deschedule_xenvif(struct xenvif *vif)
{
	struct xen_netbk *netbk = vif->netbk;
	spin_lock_irq(&netbk->net_schedule_list_lock);
	remove_from_net_schedule_list(vif);
	spin_unlock_irq(&netbk->net_schedule_list_lock);
}

void xen_netbk_check_rx_xenvif(struct xenvif *vif)
{
	int more_to_do;

	RING_FINAL_CHECK_FOR_REQUESTS(&vif->tx, more_to_do);

	if (more_to_do)
		xen_netbk_schedule_xenvif(vif);
}

static void tx_add_credit(struct xenvif *vif)
{
	unsigned long max_burst, max_credit;

	/*
	 * Allow a burst big enough to transmit a jumbo packet of up to 128kB.
	 * Otherwise the interface can seize up due to insufficient credit.
	 */
	max_burst = RING_GET_REQUEST(&vif->tx, vif->tx.req_cons)->size;
	max_burst = min(max_burst, 131072UL);
	max_burst = max(max_burst, vif->credit_bytes);

	/* Take care that adding a new chunk of credit doesn't wrap to zero. */
	max_credit = vif->remaining_credit + vif->credit_bytes;
	if (max_credit < vif->remaining_credit)
		max_credit = ULONG_MAX; /* wrapped: clamp to ULONG_MAX */

	vif->remaining_credit = min(max_credit, max_burst);
}

static void tx_credit_callback(unsigned long data)
{
	struct xenvif *vif = (struct xenvif *)data;
	tx_add_credit(vif);
	xen_netbk_check_rx_xenvif(vif);
}

/*VATC*/
static void tx_token_callback(unsigned long data)
{
	struct xenvif *vif = (struct xenvif *)data;
	//printk("tx_token_callback~~~~\n");
	xen_netbk_check_rx_xenvif(vif);
}


static void netbk_tx_err(struct xenvif *vif,
			 struct xen_netif_tx_request *txp, RING_IDX end)
{
	RING_IDX cons = vif->tx.req_cons;

	do {
		make_tx_response(vif, txp, XEN_NETIF_RSP_ERROR);
		if (cons >= end)
			break;
		txp = RING_GET_REQUEST(&vif->tx, cons++);
	} while (1);
	vif->tx.req_cons = cons;
	xen_netbk_check_rx_xenvif(vif);
	xenvif_put(vif);
}

static int netbk_count_requests(struct xenvif *vif,
				struct xen_netif_tx_request *first,
				struct xen_netif_tx_request *txp,
				int work_to_do)
{
	RING_IDX cons = vif->tx.req_cons;
	int frags = 0;

	if (!(first->flags & XEN_NETTXF_more_data))
		return 0;

	do {
		if (frags >= work_to_do) {
			netdev_dbg(vif->dev, "Need more frags\n");
			return -frags;
		}

		if (unlikely(frags >= MAX_SKB_FRAGS)) {
			netdev_dbg(vif->dev, "Too many frags\n");
			return -frags;
		}

		memcpy(txp, RING_GET_REQUEST(&vif->tx, cons + frags),
		       sizeof(*txp));
		if (txp->size > first->size) {
			netdev_dbg(vif->dev, "Frags galore\n");
			return -frags;
		}

		first->size -= txp->size;
		frags++;

		if (unlikely((txp->offset + txp->size) > PAGE_SIZE)) {
			netdev_dbg(vif->dev, "txp->offset: %x, size: %u\n",
				 txp->offset, txp->size);
			return -frags;
		}
	} while ((txp++)->flags & XEN_NETTXF_more_data);
	return frags;
}

static struct page *xen_netbk_alloc_page(struct xen_netbk *netbk,
					 struct sk_buff *skb,
					 u16 pending_idx)
{
	struct page *page;
	page = alloc_page(GFP_KERNEL|__GFP_COLD);
	if (!page)
		return NULL;
	set_page_ext(page, netbk, pending_idx);
	netbk->mmap_pages[pending_idx] = page;
	return page;
}

static struct gnttab_copy *xen_netbk_get_requests(struct xen_netbk *netbk,
						  struct xenvif *vif,
						  struct sk_buff *skb,
						  struct xen_netif_tx_request *txp,
						  struct gnttab_copy *gop)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	skb_frag_t *frags = shinfo->frags;
	u16 pending_idx = *((u16 *)skb->data);
	int i, start;

	/* Skip first skb fragment if it is on same page as header fragment. */
	start = (frag_get_pending_idx(&shinfo->frags[0]) == pending_idx);

	for (i = start; i < shinfo->nr_frags; i++, txp++) {
		struct page *page;
		pending_ring_idx_t index;
		struct pending_tx_info *pending_tx_info =
			netbk->pending_tx_info;

		index = pending_index(netbk->pending_cons++);
		pending_idx = netbk->pending_ring[index];
		page = xen_netbk_alloc_page(netbk, skb, pending_idx);
		if (!page)
			return NULL;

		gop->source.u.ref = txp->gref;
		gop->source.domid = vif->domid;
		gop->source.offset = txp->offset;

		gop->dest.u.gmfn = virt_to_mfn(page_address(page));
		gop->dest.domid = DOMID_SELF;
		gop->dest.offset = txp->offset;

		gop->len = txp->size;
		gop->flags = GNTCOPY_source_gref;

		gop++;

		memcpy(&pending_tx_info[pending_idx].req, txp, sizeof(*txp));
		xenvif_get(vif);
		pending_tx_info[pending_idx].vif = vif;
		frag_set_pending_idx(&frags[i], pending_idx);
	}

	return gop;
}

static int xen_netbk_tx_check_gop(struct xen_netbk *netbk,
				  struct sk_buff *skb,
				  struct gnttab_copy **gopp)
{
	struct gnttab_copy *gop = *gopp;
	u16 pending_idx = *((u16 *)skb->data);
	struct pending_tx_info *pending_tx_info = netbk->pending_tx_info;
	struct xenvif *vif = pending_tx_info[pending_idx].vif;
	struct xen_netif_tx_request *txp;
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int nr_frags = shinfo->nr_frags;
	int i, err, start;

	/* Check status of header. */
	err = gop->status;
	if (unlikely(err)) {
		pending_ring_idx_t index;
		index = pending_index(netbk->pending_prod++);
		txp = &pending_tx_info[pending_idx].req;
		make_tx_response(vif, txp, XEN_NETIF_RSP_ERROR);
		netbk->pending_ring[index] = pending_idx;
		xenvif_put(vif);
	}

	/* Skip first skb fragment if it is on same page as header fragment. */
	start = (frag_get_pending_idx(&shinfo->frags[0]) == pending_idx);

	for (i = start; i < nr_frags; i++) {
		int j, newerr;
		pending_ring_idx_t index;

		pending_idx = frag_get_pending_idx(&shinfo->frags[i]);

		/* Check error status: if okay then remember grant handle. */
		newerr = (++gop)->status;
		if (likely(!newerr)) {
			/* Had a previous error? Invalidate this fragment. */
			if (unlikely(err))
				xen_netbk_idx_release(netbk, pending_idx);
			continue;
		}

		/* Error on this fragment: respond to client with an error. */
		txp = &netbk->pending_tx_info[pending_idx].req;
		make_tx_response(vif, txp, XEN_NETIF_RSP_ERROR);
		index = pending_index(netbk->pending_prod++);
		netbk->pending_ring[index] = pending_idx;
		xenvif_put(vif);

		/* Not the first error? Preceding frags already invalidated. */
		if (err)
			continue;

		/* First error: invalidate header and preceding fragments. */
		pending_idx = *((u16 *)skb->data);
		xen_netbk_idx_release(netbk, pending_idx);
		for (j = start; j < i; j++) {
			pending_idx = frag_get_pending_idx(&shinfo->frags[j]);
			xen_netbk_idx_release(netbk, pending_idx);
		}

		/* Remember the error: invalidate all subsequent fragments. */
		err = newerr;
	}

	*gopp = gop + 1;
	return err;
}

static void xen_netbk_fill_frags(struct xen_netbk *netbk, struct sk_buff *skb)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int nr_frags = shinfo->nr_frags;
	int i;

	for (i = 0; i < nr_frags; i++) {
		skb_frag_t *frag = shinfo->frags + i;
		struct xen_netif_tx_request *txp;
		struct page *page;
		u16 pending_idx;

		pending_idx = frag_get_pending_idx(frag);

		txp = &netbk->pending_tx_info[pending_idx].req;
		page = virt_to_page(idx_to_kaddr(netbk, pending_idx));
		__skb_fill_page_desc(skb, i, page, txp->offset, txp->size);
		skb->len += txp->size;
		skb->data_len += txp->size;
		skb->truesize += txp->size;

		/* Take an extra reference to offset xen_netbk_idx_release */
		get_page(netbk->mmap_pages[pending_idx]);
		xen_netbk_idx_release(netbk, pending_idx);
	}
}

static int xen_netbk_get_extras(struct xenvif *vif,
				struct xen_netif_extra_info *extras,
				int work_to_do)
{
	struct xen_netif_extra_info extra;
	RING_IDX cons = vif->tx.req_cons;

	do {
		if (unlikely(work_to_do-- <= 0)) {
			netdev_dbg(vif->dev, "Missing extra info\n");
			return -EBADR;
		}

		memcpy(&extra, RING_GET_REQUEST(&vif->tx, cons),
		       sizeof(extra));
		if (unlikely(!extra.type ||
			     extra.type >= XEN_NETIF_EXTRA_TYPE_MAX)) {
			vif->tx.req_cons = ++cons;
			netdev_dbg(vif->dev,
				   "Invalid extra type: %d\n", extra.type);
			return -EINVAL;
		}

		memcpy(&extras[extra.type - 1], &extra, sizeof(extra));
		vif->tx.req_cons = ++cons;
	} while (extra.flags & XEN_NETIF_EXTRA_FLAG_MORE);

	return work_to_do;
}

static int netbk_set_skb_gso(struct xenvif *vif,
			     struct sk_buff *skb,
			     struct xen_netif_extra_info *gso)
{
	if (!gso->u.gso.size) {
		netdev_dbg(vif->dev, "GSO size must not be zero.\n");
		return -EINVAL;
	}

	/* Currently only TCPv4 S.O. is supported. */
	if (gso->u.gso.type != XEN_NETIF_GSO_TYPE_TCPV4) {
		netdev_dbg(vif->dev, "Bad GSO type %d.\n", gso->u.gso.type);
		return -EINVAL;
	}

	skb_shinfo(skb)->gso_size = gso->u.gso.size;
	skb_shinfo(skb)->gso_type = SKB_GSO_TCPV4;

	/* Header must be checked, and gso_segs computed. */
	skb_shinfo(skb)->gso_type |= SKB_GSO_DODGY;
	skb_shinfo(skb)->gso_segs = 0;

	return 0;
}

static int checksum_setup(struct xenvif *vif, struct sk_buff *skb)
{
	struct iphdr *iph;
	unsigned char *th;
	int err = -EPROTO;
	int recalculate_partial_csum = 0;

	/*
	 * A GSO SKB must be CHECKSUM_PARTIAL. However some buggy
	 * peers can fail to set NETRXF_csum_blank when sending a GSO
	 * frame. In this case force the SKB to CHECKSUM_PARTIAL and
	 * recalculate the partial checksum.
	 */
	if (skb->ip_summed != CHECKSUM_PARTIAL && skb_is_gso(skb)) {
		vif->rx_gso_checksum_fixup++;
		skb->ip_summed = CHECKSUM_PARTIAL;
		recalculate_partial_csum = 1;
	}

	/* A non-CHECKSUM_PARTIAL SKB does not require setup. */
	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;

	if (skb->protocol != htons(ETH_P_IP))
		goto out;

	iph = (void *)skb->data;
	th = skb->data + 4 * iph->ihl;
	if (th >= skb_tail_pointer(skb))
		goto out;

	skb->csum_start = th - skb->head;
	switch (iph->protocol) {
	case IPPROTO_TCP:
		skb->csum_offset = offsetof(struct tcphdr, check);

		if (recalculate_partial_csum) {
			struct tcphdr *tcph = (struct tcphdr *)th;
			tcph->check = ~csum_tcpudp_magic(iph->saddr, iph->daddr,
							 skb->len - iph->ihl*4,
							 IPPROTO_TCP, 0);
		}
		break;
	case IPPROTO_UDP:
		skb->csum_offset = offsetof(struct udphdr, check);

		if (recalculate_partial_csum) {
			struct udphdr *udph = (struct udphdr *)th;
			udph->check = ~csum_tcpudp_magic(iph->saddr, iph->daddr,
							 skb->len - iph->ihl*4,
							 IPPROTO_UDP, 0);
		}
		break;
	default:
		if (net_ratelimit())
			netdev_err(vif->dev,
				   "Attempting to checksum a non-TCP/UDP packet, dropping a protocol %d packet\n",
				   iph->protocol);
		goto out;
	}

	if ((th + skb->csum_offset + 2) > skb_tail_pointer(skb))
		goto out;

	err = 0;

out:
	return err;
}

static bool tx_credit_exceeded(struct xenvif *vif, unsigned size)
{
	unsigned long now = jiffies;
	unsigned long next_credit =
		vif->credit_timeout.expires +
		msecs_to_jiffies(vif->credit_usec / 1000);

	/* Timer could already be pending in rare cases. */
	if (timer_pending(&vif->credit_timeout))
		return true;

	/* Passed the point where we can replenish credit? */
	if (time_after_eq(now, next_credit)) {
		vif->credit_timeout.expires = now;
		tx_add_credit(vif);
	}

	/* Still too big to send right now? Set a callback. */
	if (size > vif->remaining_credit) {
		vif->credit_timeout.data     =
			(unsigned long)vif;
		vif->credit_timeout.function =
			tx_credit_callback;
		mod_timer(&vif->credit_timeout,
			  next_credit);

		return true;
	}

	return false;
}

/*VATC*/
static bool tx_token_exceeded(struct xenvif *vif, unsigned size)
{
	if (timer_pending(&vif->token_timeout))
		return true;
	
	unsigned long now = jiffies;
	if (time_after_eq(now, vif->last_fill)) {
		unsigned int elapse = jiffies_to_msecs(now-vif->last_fill);
		if (size > vif->credit_usec) {
			unsigned long toAdd = vif->credit_bytes *  elapse;
			vif->remaining_credit= vif->remaining_credit + toAdd;
			vif->last_fill = now;
		} else if (elapse > 1 || elapse == 1) {
			unsigned long toAdd = vif->credit_bytes *  elapse;
			vif->remaining_credit= min(vif->remaining_credit + toAdd, vif->credit_usec);
			vif->last_fill = now;
		}
	}
		
	if (vif->remaining_credit < size) {
		unsigned long deficit = size - vif->remaining_credit;
		vif->token_timeout.data     =
			(unsigned long)vif;
		vif->token_timeout.function =
			tx_token_callback;
		mod_timer(&vif->token_timeout,
			  vif->last_fill + msecs_to_jiffies(max(deficit/vif->credit_bytes, 1)));
		return true;
	}
	return false;
}

/*VATC*/
static bool tx_pkt_exceeded(struct xenvif *vif, unsigned size)
{
	if (timer_pending(&vif->token_timeout))
		return true;
	
	unsigned long now = jiffies;
	if (time_after_eq(now, vif->last_fill)) {
		unsigned int elapse = jiffies_to_msecs(now-vif->last_fill);
		if (size > vif->credit_usec) {
			unsigned long toAdd = vif->credit_bytes *  elapse;
			vif->remaining_credit= vif->remaining_credit + toAdd;
			vif->last_fill = now;
		} else if (elapse > 1 || elapse == 1) {
			unsigned long toAdd = vif->credit_bytes *  elapse;
			vif->remaining_credit= min(vif->remaining_credit + toAdd, vif->credit_usec);
			vif->last_fill = now;
		}
	}
		
	if (vif->remaining_credit < size) {
		unsigned long deficit = size - vif->remaining_credit;
		vif->token_timeout.data     =
			(unsigned long)vif;
		vif->token_timeout.function =
			tx_token_callback;
		mod_timer(&vif->token_timeout,
			  vif->last_fill + msecs_to_jiffies(max(deficit/vif->credit_bytes, 1)));
		return true;
	}
	return false;
}


static inline int rx_work_todo(struct xen_netbk *netbk)
{
	return !skb_queue_empty(&netbk->rx_queue);
}

static inline int tx_work_todo(struct xen_netbk *netbk)
{

	if (((nr_pending_reqs(netbk) + MAX_SKB_FRAGS) < MAX_PENDING_REQS) &&
			!list_empty(&netbk->net_schedule_list))
		return 1;

	return 0;
}

static unsigned xen_netbk_tx_build_gops(struct xen_netbk *netbk)
{
	struct gnttab_copy *gop = netbk->tx_copy_ops, *request_gop;
	struct sk_buff *skb;
	int ret;

	while (((nr_pending_reqs(netbk) + MAX_SKB_FRAGS) < MAX_PENDING_REQS) &&
		!list_empty(&netbk->net_schedule_list)) {
		struct xenvif *vif;
		struct xen_netif_tx_request txreq;
		struct xen_netif_tx_request txfrags[MAX_SKB_FRAGS];
		struct page *page;
		struct xen_netif_extra_info extras[XEN_NETIF_EXTRA_TYPE_MAX-1];
		u16 pending_idx;
		RING_IDX idx;
		int work_to_do;
		unsigned int data_len;
		pending_ring_idx_t index;

		/* Get a netif from the list with work to do. */
		vif = poll_net_schedule_list(netbk);
		if (!vif)
			continue;

		RING_FINAL_CHECK_FOR_REQUESTS(&vif->tx, work_to_do);
		if (!work_to_do) {
			xenvif_put(vif);
			continue;
		}

		idx = vif->tx.req_cons;
		rmb(); /* Ensure that we see the request before we copy it. */
		memcpy(&txreq, RING_GET_REQUEST(&vif->tx, idx), sizeof(txreq));

		/*VATC*/
		if (vif->limit_type == 0) {
			/* Credit-based scheduling. */
			if (txreq.size > vif->remaining_credit &&
		    		tx_credit_exceeded(vif, txreq.size)) {
				xenvif_put(vif);
				continue;
			}
			vif->remaining_credit -= txreq.size;
		} else if (vif->limit_type == 1) {
			/*VATC token bucket*/
			if (vif->credit_usec == 0) {
				goto notb;
			}
			if (tx_token_exceeded(vif, txreq.size)) {
				xenvif_put(vif);
				//printk("lack tokens~~~~\n");
				continue;
			}
			vif->remaining_credit -= txreq.size;
		} else {
			/*VATC pkt-based token bucket*/
			if (vif->credit_usec == 0) {
				goto notb;
			}
			if (tx_pkt_exceeded(vif, 1)) {
				xenvif_put(vif);
				continue;
			}
			vif->remaining_credit -= 1;
		}
notb:
				
		work_to_do--;
		vif->tx.req_cons = ++idx;

		memset(extras, 0, sizeof(extras));
		if (txreq.flags & XEN_NETTXF_extra_info) {
			work_to_do = xen_netbk_get_extras(vif, extras,
							  work_to_do);
			idx = vif->tx.req_cons;
			if (unlikely(work_to_do < 0)) {
				netbk_tx_err(vif, &txreq, idx);
				continue;
			}
		}

		ret = netbk_count_requests(vif, &txreq, txfrags, work_to_do);
		if (unlikely(ret < 0)) {
			netbk_tx_err(vif, &txreq, idx - ret);
			continue;
		}
		idx += ret;

		if (unlikely(txreq.size < ETH_HLEN)) {
			netdev_dbg(vif->dev,
				   "Bad packet size: %d\n", txreq.size);
			netbk_tx_err(vif, &txreq, idx);
			continue;
		}

		/* No crossing a page as the payload mustn't fragment. */
		if (unlikely((txreq.offset + txreq.size) > PAGE_SIZE)) {
			netdev_dbg(vif->dev,
				   "txreq.offset: %x, size: %u, end: %lu\n",
				   txreq.offset, txreq.size,
				   (txreq.offset&~PAGE_MASK) + txreq.size);
			netbk_tx_err(vif, &txreq, idx);
			continue;
		}

		index = pending_index(netbk->pending_cons);
		pending_idx = netbk->pending_ring[index];

		data_len = (txreq.size > PKT_PROT_LEN &&
			    ret < MAX_SKB_FRAGS) ?
			PKT_PROT_LEN : txreq.size;

		skb = alloc_skb(data_len + NET_SKB_PAD + NET_IP_ALIGN,
				GFP_ATOMIC | __GFP_NOWARN);
		if (unlikely(skb == NULL)) {
			netdev_dbg(vif->dev,
				   "Can't allocate a skb in start_xmit.\n");
			netbk_tx_err(vif, &txreq, idx);
			break;
		}

		/* Packets passed to netif_rx() must have some headroom. */
		skb_reserve(skb, NET_SKB_PAD + NET_IP_ALIGN);

		if (extras[XEN_NETIF_EXTRA_TYPE_GSO - 1].type) {
			struct xen_netif_extra_info *gso;
			gso = &extras[XEN_NETIF_EXTRA_TYPE_GSO - 1];

			if (netbk_set_skb_gso(vif, skb, gso)) {
				kfree_skb(skb);
				netbk_tx_err(vif, &txreq, idx);
				continue;
			}
		}

		/* XXX could copy straight to head */
		page = xen_netbk_alloc_page(netbk, skb, pending_idx);
		if (!page) {
			kfree_skb(skb);
			netbk_tx_err(vif, &txreq, idx);
			continue;
		}

		gop->source.u.ref = txreq.gref;
		gop->source.domid = vif->domid;
		gop->source.offset = txreq.offset;

		gop->dest.u.gmfn = virt_to_mfn(page_address(page));
		gop->dest.domid = DOMID_SELF;
		gop->dest.offset = txreq.offset;

		gop->len = txreq.size;
		gop->flags = GNTCOPY_source_gref;

		gop++;

		memcpy(&netbk->pending_tx_info[pending_idx].req,
		       &txreq, sizeof(txreq));
		netbk->pending_tx_info[pending_idx].vif = vif;
		*((u16 *)skb->data) = pending_idx;

		__skb_put(skb, data_len);

		skb_shinfo(skb)->nr_frags = ret;
		if (data_len < txreq.size) {
			skb_shinfo(skb)->nr_frags++;
			frag_set_pending_idx(&skb_shinfo(skb)->frags[0],
					     pending_idx);
		} else {
			frag_set_pending_idx(&skb_shinfo(skb)->frags[0],
					     INVALID_PENDING_IDX);
		}

		__skb_queue_tail(&netbk->tx_queue, skb);

		netbk->pending_cons++;

		request_gop = xen_netbk_get_requests(netbk, vif,
						     skb, txfrags, gop);
		if (request_gop == NULL) {
			kfree_skb(skb);
			netbk_tx_err(vif, &txreq, idx);
			continue;
		}
		gop = request_gop;

		vif->tx.req_cons = idx;
		xen_netbk_check_rx_xenvif(vif);

		if ((gop-netbk->tx_copy_ops) >= ARRAY_SIZE(netbk->tx_copy_ops))
			break;

	}

	return gop - netbk->tx_copy_ops;
}

static void xen_netbk_tx_submit(struct xen_netbk *netbk)
{
	struct gnttab_copy *gop = netbk->tx_copy_ops;
	struct sk_buff *skb;
	struct ethhdr *eth_header;
	struct iphdr * ip_header;
	struct softnet_data *sd;
	sd=&__get_cpu_var(softnet_data);
	int i;
	int netbk_index=0;
	int vif_index;
	int qlen=netbk->tx_queue.qlen;
	int rc;

	while ((skb = __skb_dequeue(&netbk->tx_queue)) != NULL) {
/*		
#ifdef NEW_NETBACK
		if(BQL_flag==0||DQL_flag==0){			
			__skb_queue_head(&netbk->tx_queue, skb);
			
			break;
		}
		else
			rcu_read_lock();

#endif
*/
		struct xen_netif_tx_request *txp;
		struct xenvif *vif;
		u16 pending_idx;
		unsigned data_len;

		pending_idx = *((u16 *)skb->data);
		vif = netbk->pending_tx_info[pending_idx].vif;
		txp = &netbk->pending_tx_info[pending_idx].req;

		/* Check the remap error code. */
		if (unlikely(xen_netbk_tx_check_gop(netbk, skb, &gop))) {
			netdev_dbg(vif->dev, "netback grant failed.\n");
			skb_shinfo(skb)->nr_frags = 0;
			kfree_skb(skb);
/*
#ifdef NEW_NETBACK
			rcu_read_unlock();
#endif
*/
			continue;
		}

		data_len = skb->len;
		memcpy(skb->data,
		       (void *)(idx_to_kaddr(netbk, pending_idx)|txp->offset),
		       data_len);
		if (data_len < txp->size) {
			/* Append the packet payload as a fragment. */
			txp->offset += data_len;
			txp->size -= data_len;
		} else {
			/* Schedule a response immediately. */
			xen_netbk_idx_release(netbk, pending_idx);
		}

		if (txp->flags & XEN_NETTXF_csum_blank)
			skb->ip_summed = CHECKSUM_PARTIAL;
		else if (txp->flags & XEN_NETTXF_data_validated)
			skb->ip_summed = CHECKSUM_UNNECESSARY;

		xen_netbk_fill_frags(netbk, skb);

		/*
		 * If the initial fragment was < PKT_PROT_LEN then
		 * pull through some bytes from the other fragments to
		 * increase the linear region to PKT_PROT_LEN bytes.
		 */
		if (skb_headlen(skb) < PKT_PROT_LEN && skb_is_nonlinear(skb)) {
			int target = min_t(int, skb->len, PKT_PROT_LEN);
			__pskb_pull_tail(skb, target - skb_headlen(skb));
		}

		skb->dev      = vif->dev;
		skb->protocol = eth_type_trans(skb, skb->dev);

		if (checksum_setup(vif, skb)) {
			netdev_dbg(vif->dev,
				   "Can't setup checksum in net_tx_action\n");
			kfree_skb(skb);
/*
#ifdef NEW_NETBACK
			rcu_read_unlock();
#endif
*/
			continue;
		}

		vif->dev->stats.rx_bytes += skb->len;
		vif->dev->stats.rx_packets++;

		
		eth_header=(struct ethhdr *)skb_mac_header(skb);

/*RTCA*/		
#ifdef NEW_NETBACK
		
			/*eth_header=(struct ethhdr *)skb_mac_header(skb);
			ip_header=(struct iphdr *)((char *)eth_header+sizeof(struct ethhdr));
			
			
			for(i=0;i<sd->dom_index;i++){
				if(ether_addr_equal(eth_header->h_dest,sd->localdoms[i]))
					break;
			}

			if(i<sd->dom_index){
				if(eth_header->h_proto!=htons(ETH_P_ARP)){
						memcpy(&(skb->cb[40]), "vif",3); //this is a packet coming from guest domain
						skb->cb[43]=skb->dev->domid+'0';

						skb->dev=sd->dev_queue[i];
						skb_push(skb, ETH_HLEN);
						dev_queue_xmit(skb);
				}
				else{
					netif_receive_skb(skb);
				}
			}
			else{
				if(eth_header->h_proto!=htons(ETH_P_ARP)){
						memcpy(&(skb->cb[40]), "vif",3); //this is a packet coming from guest domain
						skb->cb[43]=skb->dev->domid+'0';
						skb_reset_network_header(skb);
						skb_reset_transport_header(skb);
						skb_reset_mac_len(skb);
						skb->dev=NIC_dev;
						skb_push(skb, ETH_HLEN);
						rc=dev_queue_xmit(skb);
						if(rc==110){
							netbk->gso_skb=skb;
							netbk->gso_flag=1;
							rcu_read_unlock();
							break;
						}
				}
				else{
					netif_receive_skb(skb);
				}
			}*/
/*			rc=netif_receive_skb(skb);
			if(rc==110){
				netbk->gso_skb=skb;
				netbk->gso_flag=1;
				rcu_read_unlock();
				break;
			}

normal:
			rcu_read_unlock();			
			continue;
*/
#endif


/*VATC*/
		//rcu_read_lock();
		netif_receive_skb(skb);
		//rcu_read_unlock();
	
#ifndef NEW_NETBACK
		xenvif_receive_skb(vif, skb);
#endif
		
	}

}

/* Called after netfront has transmitted */
static void xen_netbk_tx_action(struct xen_netbk *netbk)
{
	unsigned nr_gops;
	int ret;

	nr_gops = xen_netbk_tx_build_gops(netbk);

/*
#ifdef NEW_NETBACK
	goto going;
#endif
*/
	if (nr_gops == 0)
		return;
going:
	ret = HYPERVISOR_grant_table_op(GNTTABOP_copy,
					netbk->tx_copy_ops, nr_gops);
	BUG_ON(ret);
submit:
	xen_netbk_tx_submit(netbk);

}


static void xen_netbk_idx_release(struct xen_netbk *netbk, u16 pending_idx)
{
	struct xenvif *vif;
	struct pending_tx_info *pending_tx_info;
	pending_ring_idx_t index;

	/* Already complete? */
	if (netbk->mmap_pages[pending_idx] == NULL)
		return;

	pending_tx_info = &netbk->pending_tx_info[pending_idx];

	vif = pending_tx_info->vif;

	make_tx_response(vif, &pending_tx_info->req, XEN_NETIF_RSP_OKAY);

	index = pending_index(netbk->pending_prod++);
	netbk->pending_ring[index] = pending_idx;

	xenvif_put(vif);

	netbk->mmap_pages[pending_idx]->mapping = 0;
	put_page(netbk->mmap_pages[pending_idx]);
	netbk->mmap_pages[pending_idx] = NULL;
}

static void make_tx_response(struct xenvif *vif,
			     struct xen_netif_tx_request *txp,
			     s8       st)
{
	RING_IDX i = vif->tx.rsp_prod_pvt;
	struct xen_netif_tx_response *resp;
	int notify;

	resp = RING_GET_RESPONSE(&vif->tx, i);
	resp->id     = txp->id;
	resp->status = st;

	if (txp->flags & XEN_NETTXF_extra_info)
		RING_GET_RESPONSE(&vif->tx, ++i)->status = XEN_NETIF_RSP_NULL;

	vif->tx.rsp_prod_pvt = ++i;
	RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&vif->tx, notify);
	if (notify)
		notify_remote_via_irq(vif->irq);
}

static struct xen_netif_rx_response *make_rx_response(struct xenvif *vif,
					     u16      id,
					     s8       st,
					     u16      offset,
					     u16      size,
					     u16      flags)
{
	RING_IDX i = vif->rx.rsp_prod_pvt;
	struct xen_netif_rx_response *resp;

	resp = RING_GET_RESPONSE(&vif->rx, i);
	resp->offset     = offset;
	resp->flags      = flags;
	resp->id         = id;
	resp->status     = (s16)size;
	if (st < 0)
		resp->status = (s16)st;

	vif->rx.rsp_prod_pvt = ++i;

	return resp;
}

static int xen_netbk_kthread(void *data)
{
	struct xen_netbk *netbk = data;
	
	
	while (!kthread_should_stop()) {
		wait_event_interruptible(netbk->wq,
				rx_work_todo(netbk) ||
				tx_work_todo(netbk) ||
				kthread_should_stop());
		cond_resched();

		if (kthread_should_stop())
			break;

		if (rx_work_todo(netbk)){
			//if(netbk->priority==0)
				//printk("pkt0 into rx_action\n");
			xen_netbk_rx_action(netbk);
		}

		if (tx_work_todo(netbk)){
			xen_netbk_tx_action(netbk);
		}
	}

	return 0;
}


/*RTCA*/
/*static int rtca_netbk_kthread(void *data)
{
	struct xen_netbk *netbk = data;
	
	int kthread_priority=96-netbk->priority;
	printk("!!!!!!!~kthread_priority=%d !!!!!!!!\n", kthread_priority);
	struct sched_param net_recv_param={.sched_priority=kthread_priority};
	sched_setscheduler(current,SCHED_FIFO,&net_recv_param);

	
	while (!kthread_should_stop()) {
		//netbk->wq
		wait_event_interruptible(netbk->wq,
				(rx_work_todo(netbk)||(netbk->vif!=NULL&&netbk->vif->rx_queue_backup.qlen>0&&xenvif_rx_schedulable(netbk->vif))) ||//!test_bit(__QUEUE_STATE_STACK_XOFF, &((struct netdev_queue *)(&NIC_dev->_tx[0]))->state))
				((tx_work_todo(netbk) ||netbk->tx_queue.qlen>0||netbk->gso_skb)) ||
				kthread_should_stop());
		wait_event_interruptible(netbk->tx_wq, ((BQL_flag==1)&&(DQL_flag==1))||(rx_work_todo(netbk)||(netbk->vif!=NULL&&netbk->vif->rx_queue_backup.qlen>0&&xenvif_rx_schedulable(netbk->vif))));
		cond_resched();


		if (kthread_should_stop())
			break;

		if (rx_work_todo(netbk)){
			xen_netbk_rx_action(netbk);
		}
		if(netbk->vif!=NULL&&netbk->vif->rx_queue_backup.qlen>0&&xenvif_rx_schedulable(netbk->vif)){
			struct sk_buff *skb;
			while(netbk->vif->rx_queue_backup.qlen>0){
				if (!xenvif_rx_schedulable(netbk->vif))
					break;	
				skb=skb_dequeue(&netbk->vif->rx_queue_backup);
				netbk->vif->rx_req_cons_peek += xen_netbk_count_skb_slots(netbk->vif,skb );
				xenvif_get(netbk->vif);
				skb_queue_tail(&netbk->rx_queue, skb);
			}
			xen_netbk_rx_action(netbk);
		}
		if(netbk->gso_skb && (BQL_flag==1) && (DQL_flag==1)){
				netbk->gso_flag=0;
				rcu_read_lock();
				rcu_read_lock_bh();
				struct sk_buff *skb2=netbk->gso_skb;
				netbk->gso_skb=NULL;
				int rc; 
				//struct sk_buff* skb;
				rc=dev_hard_start_xmit(skb2, NIC_dev, &NIC_dev->_tx[0]);
				//rc=dev_queue_xmit(skb2);
				rcu_read_unlock_bh();
				rcu_read_unlock();
				
				if(rc==110){
					netbk->gso_skb=skb2;
					netbk->gso_flag=1;
					continue;
				}
				
		}
		if ((tx_work_todo(netbk)||netbk->tx_queue.qlen>0)&&(BQL_flag==1)&&(DQL_flag==1)){
			xen_netbk_tx_action(netbk);
		}
		
	}

	return 0;
}*/

/*VATC*/
static int rtca_netbk_kthread(void *data)
{
	struct xen_netbk *netbk = data;
	
	int kthread_priority=96-netbk->priority;
	printk("!!!!!!!~kthread_priority=%d !!!!!!!!\n", kthread_priority);
	struct sched_param net_recv_param={.sched_priority=kthread_priority};
	sched_setscheduler(current,SCHED_FIFO,&net_recv_param);

	
	while (!kthread_should_stop()) {
		wait_event_interruptible(netbk->wq,
				rx_work_todo(netbk) ||tx_work_todo(netbk) ||
				kthread_should_stop());
		//wait_event_interruptible(netbk->tx_wq, ((BQL_flag==1)&&(DQL_flag==1))||(rx_work_todo(netbk)||(netbk->vif!=NULL&&netbk->vif->rx_queue_backup.qlen>0&&xenvif_rx_schedulable(netbk->vif))));
		cond_resched();


		if (kthread_should_stop())
			break;

		if (rx_work_todo(netbk)){
			xen_netbk_rx_action(netbk);
		}
		if (tx_work_todo(netbk)){
			xen_netbk_tx_action(netbk);
		}		
	}

	return 0;
}


void xen_netbk_unmap_frontend_rings(struct xenvif *vif)
{
	if (vif->tx.sring)
		xenbus_unmap_ring_vfree(xenvif_to_xenbus_device(vif),
					vif->tx.sring);
	if (vif->rx.sring)
		xenbus_unmap_ring_vfree(xenvif_to_xenbus_device(vif),
					vif->rx.sring);
}

int xen_netbk_map_frontend_rings(struct xenvif *vif,
				 grant_ref_t tx_ring_ref,
				 grant_ref_t rx_ring_ref)
{
	void *addr;
	struct xen_netif_tx_sring *txs;
	struct xen_netif_rx_sring *rxs;

	int err = -ENOMEM;

	err = xenbus_map_ring_valloc(xenvif_to_xenbus_device(vif),
				     tx_ring_ref, &addr);
	if (err)
		goto err;

	txs = (struct xen_netif_tx_sring *)addr;
	BACK_RING_INIT(&vif->tx, txs, PAGE_SIZE);

	err = xenbus_map_ring_valloc(xenvif_to_xenbus_device(vif),
				     rx_ring_ref, &addr);
	if (err)
		goto err;

	rxs = (struct xen_netif_rx_sring *)addr;
	BACK_RING_INIT(&vif->rx, rxs, PAGE_SIZE);

	vif->rx_req_cons_peek = 0;

	return 0;

err:
	xen_netbk_unmap_frontend_rings(vif);
	return err;
}

static int __init netback_init(void)
{
	int i;
	int rc = 0;
	int group;

	if (!xen_domain())
		return -ENODEV;

	xen_netbk_group_nr = num_online_cpus();

/*RTCA*/
#ifdef NEW_NETBACK
	xen_netbk_group_nr=6;
#endif
	xen_netbk = vzalloc(sizeof(struct xen_netbk) * xen_netbk_group_nr);
	if (!xen_netbk) {
		printk(KERN_ALERT "%s: out of memory\n", __func__);
		return -ENOMEM;
	}

	for (group = 0; group < xen_netbk_group_nr; group++) {
		printk("~~~~~~\n ~~~~~~~\n ~~~~~~~\n group==%d ~~~~~~~~~~~~~~~~~\n", group);
		struct xen_netbk *netbk = &xen_netbk[group];
		skb_queue_head_init(&netbk->rx_queue);
		skb_queue_head_init(&netbk->rx_queue_backup);
		skb_queue_head_init(&netbk->tx_queue);
		skb_queue_head_init(&netbk->tx_queue_backup);
		netbk->gso_flag=0;
		netbk->gso_skb=NULL;

		init_timer(&netbk->net_timer);
		netbk->net_timer.data = (unsigned long)netbk;
		netbk->net_timer.function = xen_netbk_alarm;

		netbk->pending_cons = 0;
		netbk->pending_prod = MAX_PENDING_REQS;
/*RTCA*/
		netbk->priority=group;
		for (i = 0; i < MAX_PENDING_REQS; i++)
			netbk->pending_ring[i] = i;

		init_waitqueue_head(&netbk->wq);
		init_waitqueue_head(&netbk->tx_wq);
		netbk_wq[group]=&netbk->wq;
		netbk_tx_wq[group]=&netbk->tx_wq;
#ifndef NEW_NETBACK
		netbk->task = kthread_create(xen_netbk_kthread,
					     (void *)netbk,
					     "netback/%u", group);
#endif

#ifdef NEW_NETBACK
		netbk->task = kthread_create(rtca_netbk_kthread,
					     (void *)netbk,
					     "netback/%u", group);
#endif
		if (IS_ERR(netbk->task)) {
			printk(KERN_ALERT "kthread_create() fails at netback\n");
			del_timer(&netbk->net_timer);
			rc = PTR_ERR(netbk->task);
			goto failed_init;
		}
#ifndef NEW_NETBACK
		kthread_bind(netbk->task, group);
#endif
/*RTCA*/
#ifdef NEW_NETBACK
		kthread_bind(netbk->task, 0); //domain0 has only one VCPU
#endif

		INIT_LIST_HEAD(&netbk->net_schedule_list);

		spin_lock_init(&netbk->net_schedule_list_lock);

		atomic_set(&netbk->netfront_count, 0);

		wake_up_process(netbk->task);
	}

	rc = xenvif_xenbus_init();
	if (rc)
		goto failed_init;

	return 0;

failed_init:
	while (--group >= 0) {
		struct xen_netbk *netbk = &xen_netbk[group];
		for (i = 0; i < MAX_PENDING_REQS; i++) {
			if (netbk->mmap_pages[i])
				__free_page(netbk->mmap_pages[i]);
		}
		del_timer(&netbk->net_timer);
		kthread_stop(netbk->task);
	}
	vfree(xen_netbk);
	return rc;

}

module_init(netback_init);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_ALIAS("xen-backend:vif");
