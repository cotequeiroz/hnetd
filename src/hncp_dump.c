#include "hncp_dump.h"

#include "hncp_i.h"
#include <libubox/blobmsg_json.h>

#define hd_a(test, err) do{if(!(test)) {err;}}while(0)

static char __hexhash[HNCP_HASH_LEN*2 + 1];
#define hd_hash_to_hex(hash) hexlify(__hexhash, (hash)->buf, HNCP_HASH_LEN)

static hnetd_time_t hd_now; //time hncp_dump is called

#define hd_do_in_nested(buf, type, name, action, err) do { \
		void *__k; \
		if(!(__k =  blobmsg_open_ ## type (buf, name)) || (action)) { \
			if(__k) \
				blobmsg_close_ ## type (buf, __k);\
			do{err;}while(0);\
		}\
		blobmsg_close_ ## type (buf, __k);\
} while(0)

#define hd_do_in_array(buf, name, action, err) hd_do_in_nested(buf, array, name, action, err)
#define hd_do_in_table(buf, name, action, err) hd_do_in_nested(buf, table, name, action, err)

#define blobmsg_add_named_blob(buf, name, attr) blobmsg_add_field(buf, blobmsg_type(attr), name, \
													blobmsg_data(attr), blobmsg_data_len(attr))

static int hd_push_hex(struct blob_buf *b, const char *name, void *data, size_t data_len)
{
	char *options;
	hd_a(options = malloc(data_len*2 + 1), return -1);
	hexlify(options, data, data_len);
	options[data_len*2] = '\0';
	hd_a(!blobmsg_add_string(b, name, options), free(options); return -1;);
	free(options);
	return 0;
}

static int hd_node_address(struct tlv_attr *tlv, struct blob_buf *b)
{
	hncp_t_router_address ra = (hncp_t_router_address) tlv_data(tlv);
	if(tlv_len(tlv) != 20)
		return -1;
	hd_a(!blobmsg_add_string(b, "address", ADDR_REPR(&ra->address)), return -1);
	hd_a(!blobmsg_add_u32(b, "link-id", ntohl(ra->link_id)), return -1);
	return 0;
}

static int hd_node_externals_dp(struct tlv_attr *tlv, struct blob_buf *b)
{
	hncp_t_delegated_prefix_header dh;
	int plen;
	struct prefix p;
	if (!(dh = hncp_tlv_dp(tlv)))
		return -1;
	memset(&p, 0, sizeof(p));
	p.plen = dh->prefix_length_bits;
	plen = ROUND_BITS_TO_BYTES(p.plen);
	memcpy(&p, dh->prefix_data, plen);
	hd_a(!blobmsg_add_string(b, "prefix", PREFIX_REPR(&p)), return -1);
	hd_a(!blobmsg_add_u64(b, "valid", ntohl(dh->ms_valid_at_origination)), return -1);
	hd_a(!blobmsg_add_u64(b, "preferred", ntohl(dh->ms_preferred_at_origination)), return -1);
	return 0;
}

static int hd_node_external(struct tlv_attr *tlv, struct blob_buf *b)
{
	struct tlv_attr *a;
	struct blob_buf dps = {NULL, NULL, 0, NULL};
	int ret = -1;

	hd_a(!blob_buf_init(&dps, BLOBMSG_TYPE_ARRAY), return -1);
	tlv_for_each_attr(a, tlv)
	{
		switch (tlv_id(a)) {
			case HNCP_T_DELEGATED_PREFIX:
				hd_do_in_table(&dps, NULL, hd_node_externals_dp(a, &dps), goto err);
				break;
			case HNCP_T_DHCPV6_OPTIONS:
				hd_a(tlv_len(a) > 0, goto err);
				hd_a(!hd_push_hex(b, "dhcpv6", tlv_data(a), tlv_len(a)), goto err);
				break;
			case HNCP_T_DHCP_OPTIONS:
				hd_a(tlv_len(a) > 0, goto err);
				hd_a(!hd_push_hex(b, "dhcpv4", tlv_data(a), tlv_len(a)), goto err);
				break;
			default:
				break;
		}
	}

	hd_a(!blobmsg_add_named_blob(b, "delegated", dps.head), goto err);
	ret = 0;
err:
	blob_buf_free(&dps);
	return ret;
}

static int hd_node_neighbor(struct tlv_attr *tlv, struct blob_buf *b)
{
	hncp_t_node_data_neighbor nh;

	if (!(nh = hncp_tlv_neighbor(tlv)))
		return -1;
	hd_a(!blobmsg_add_string(b, "node-id", hd_hash_to_hex(&nh->neighbor_node_identifier_hash)), return -1);
	hd_a(!blobmsg_add_u32(b, "local-link", ntohl(nh->link_id)), return -1);
	hd_a(!blobmsg_add_u32(b, "neighbor-link", ntohl(nh->neighbor_link_id)), return -1);
	return 0;
}

static int hd_node_prefix(struct tlv_attr *tlv, struct blob_buf *b)
{
	hncp_t_assigned_prefix_header ah;
	int plen;
	struct prefix p;

	if (!(ah = hncp_tlv_ap(tlv)))
		return -1;
	memset(&p, 0, sizeof(p));
	p.plen = ah->prefix_length_bits;
	plen = ROUND_BITS_TO_BYTES(p.plen);
	memcpy(&p, ah->prefix_data, plen);
	hd_a(!blobmsg_add_string(b, "prefix", PREFIX_REPR(&p)), return -1);
	hd_a(!blobmsg_add_u8(b, "authoritative", !!(ah->flags & HNCP_T_ASSIGNED_PREFIX_FLAG_AUTHORITATIVE)), return -1);
	hd_a(!blobmsg_add_u16(b, "priority", HNCP_T_ASSIGNED_PREFIX_FLAG_PREFERENCE(ah->flags)), return -1);
	hd_a(!blobmsg_add_u32(b, "link", ntohl(ah->link_id)), return -1);
	return 0;
}

static int hd_node(hncp o, hncp_node n, struct blob_buf *b)
{
	struct tlv_attr *tlv;
	struct blob_buf prefixes = {NULL, NULL, 0, NULL},
			neighbors = {NULL, NULL, 0, NULL},
			externals = {NULL, NULL, 0, NULL},
			addresses = {NULL, NULL, 0, NULL};
	int ret = -1;

	hd_a(!blobmsg_add_u32(b, "version", n->version), return -1);
	hd_a(!blobmsg_add_u32(b, "update", n->update_number), return -1);
	hd_a(!blobmsg_add_u64(b, "age", hd_now - n->origination_time), return -1);
	if(n == o->own_node)
			hd_a(!blobmsg_add_u8(b, "self", 1), return -1);

	hd_a(!blob_buf_init(&prefixes, BLOBMSG_TYPE_ARRAY), goto px);
	hd_a(!blob_buf_init(&neighbors, BLOBMSG_TYPE_ARRAY), goto nh);
	hd_a(!blob_buf_init(&externals, BLOBMSG_TYPE_ARRAY), goto el);
	hd_a(!blob_buf_init(&addresses, BLOBMSG_TYPE_ARRAY), goto ad);

	hncp_node_for_each_tlv(n, tlv) {
		switch (tlv_id(tlv)) {
			case HNCP_T_ASSIGNED_PREFIX:
				hd_do_in_table(&prefixes, NULL, hd_node_prefix(tlv, &prefixes), goto err);
				break;
			case HNCP_T_NODE_DATA_NEIGHBOR:
				hd_do_in_table(&neighbors, NULL, hd_node_neighbor(tlv, &neighbors), goto err);
				break;
			case HNCP_T_EXTERNAL_CONNECTION:
				hd_do_in_table(&externals, NULL, hd_node_external(tlv, &externals), goto err);
				break;
			case HNCP_T_ROUTER_ADDRESS:
				hd_do_in_table(&addresses, NULL, hd_node_address(tlv, &addresses), goto err);
				break;
			default:
				break;
		}
	}

	hd_a(!blobmsg_add_named_blob(b, "neighbors", neighbors.head), goto err);
	hd_a(!blobmsg_add_named_blob(b, "prefixes", prefixes.head), goto err);
	hd_a(!blobmsg_add_named_blob(b, "uplinks", externals.head), goto err);
	hd_a(!blobmsg_add_named_blob(b, "addresses", addresses.head), goto err);
	ret = 0;
err:
	blob_buf_free(&addresses);
ad:
	blob_buf_free(&externals);
el:
	blob_buf_free(&neighbors);
nh:
	blob_buf_free(&prefixes);
px:
	return ret;
}

static int hd_nodes(hncp o, struct blob_buf *b)
{
	hncp_node node;
	vlist_for_each_element(&o->nodes, node, in_nodes)
		hd_do_in_table(b, hd_hash_to_hex(&node->node_identifier_hash), hd_node(o, node,b), return -1);
	return 0;
}

static int hd_links(hncp o, struct blob_buf *b)
{
	hncp_link link;
	vlist_for_each_element(&o->links, link, in_links)
		hd_a(!blobmsg_add_u32(b, link->ifname, link->iid), return -1);
	return 0;
}

static int hd_info(hncp o, struct blob_buf *b)
{
	hd_a(!blobmsg_add_u64(b, "time", hd_now), return -1);
	hd_a(!blobmsg_add_string(b, "node-id", hd_hash_to_hex(&o->own_node->node_identifier_hash)), return -1);
	return 0;
}


struct blob_buf *hncp_dump(hncp o)
{
	struct blob_buf *b;
	hd_now = hnetd_time();
	hd_a(b = calloc(1, sizeof(*b)), goto alloc);
	hd_a(!blob_buf_init(b, 0), goto init);
	hd_a(!hd_info(o, b), goto fill);
	hd_do_in_table(b, "links", hd_links(o,b), goto fill);
	hd_do_in_table(b, "nodes", hd_nodes(o,b), goto fill);
	return b;
fill:
	blob_buf_free(b);
init:
	free(b);
alloc:
	return NULL;
}


