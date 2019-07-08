/* for gtpu_encap decls */
#include "gtpu_encap.h"
/* for rte_zmalloc() */
#include <rte_malloc.h>
/* for IPVERSION */
#include <netinet/ip.h>
/* for be32_t */
#include "utils/endian.h"
/* for ToIpv4Address() */
#include "utils/ip.h"
/* for udp header */
#include "utils/udp.h"
#ifdef RTE_HASH
#include <rte_jhash.h>
#endif
/*----------------------------------------------------------------------------------*/
using bess::utils::Ipv4;
using bess::utils::Udp;
using bess::utils::be32_t;
using bess::utils::be16_t;
using bess::utils::ToIpv4Address;

enum {DEFAULT_GATE = 0, FORWARD_GATE};
/*----------------------------------------------------------------------------------*/
const Commands GtpuEncap::cmds = {
	{"add", "GtpuEncapAddSessionRecordArg",
	 MODULE_CMD_FUNC(&GtpuEncap::AddSessionRecord),
#ifdef RTE_HASH
	 Command::THREAD_SAFE},
#else
         Command::THREAD_UNSAFE},
#endif
	{"remove", "GtpuEncapRemoveSessionRecordArg",
	 MODULE_CMD_FUNC(&GtpuEncap::RemoveSessionRecord),
#ifdef RTE_HASH
	 Command::THREAD_SAFE},
#else
         Command::THREAD_UNSAFE},
#endif
	{"show_records", "EmptyArg",
	 MODULE_CMD_FUNC(&GtpuEncap::ShowRecords),
#ifdef RTE_HASH
	 Command::THREAD_SAFE}
#else
         Command::THREAD_UNSAFE}
#endif
};
/*----------------------------------------------------------------------------------*/
// Template for generating UDP packets without data
struct[[gnu::packed]] PacketTemplate {
	Ipv4 iph;
	Udp udph;
	struct gtpu_hdr gtph;

	PacketTemplate() {
		gtph.version = GTPU_VERSION;
		gtph.pt = GTP_PROTOCOL_TYPE_GTP;
		gtph.spare = 0;
		gtph.ex = 0;
		gtph.seq = 0;
		gtph.pdn = 0;
		gtph.type = GTP_GPDU;
		gtph.length = 0;			// to fill in
		gtph.teid = 0;				// to fill in
		udph.src_port = (be16_t)UDP_PORT_GTPU;
		udph.dst_port = (be16_t)UDP_PORT_GTPU;
		udph.length = (be16_t)0;		// to fill in
		/* calculated by L4Checksum module in line */
		udph.checksum = 0;
		iph.version = IPVERSION;
		iph.header_length = (sizeof(Ipv4) >> 2);
		iph.type_of_service = 0;
		iph.length = (be16_t)0;			// to fill in
		iph.id = (be16_t)0x513;
		iph.fragment_offset = (be16_t)0;
		iph.ttl = 64;
		iph.protocol = IPPROTO_UDP;
		/* calculated by IPChecksum module in line */
		iph.checksum = 0;
		iph.src = (be32_t)0;			// to fill in
		iph.dst = (be32_t)0;			// to fill in
	}
};
static PacketTemplate outer_ip_template;
/*----------------------------------------------------------------------------------*/
int
GtpuEncap::dp_session_create(struct session_info *entry)
{
	struct session_info *data;
#if 0
	struct ue_session_info *ue_data;
	uint32_t ue_sess_id, bear_id;

	ue_data = NULL;
	ue_sess_id = UE_SESS_ID(entry->sess_id);
	bear_id = UE_BEAR_ID(entry->sess_id);
#endif

	/* allocate memory for session info */
	data = (struct session_info *)rte_calloc("session_info",
						 sizeof(struct session_info),
						 1,
						 0); 
	if (data == NULL) {
		std::cerr << "Failed to allocate memory for session info!" << std::endl;
		return -1;
	}
#ifndef RTE_HASH
	if (session_map.Insert(entry->sess_id, (uint64_t)data) == NULL) {
		std::cerr << "Failed to insert session info with " << " sess_id "
			  << entry->sess_id << std::endl;
	}
#else
	if (rte_hash_add_key_data(session_map, &entry->sess_id, data) < 0) {
               std::cerr << "Failed to insert session info with " << " sess_id "
                         << entry->sess_id << std::endl;
	}
#endif
	/* copy session info to the entry */
	data->ue_addr = entry->ue_addr;
	data->ul_s1_info = entry->ul_s1_info;
	data->dl_s1_info = entry->dl_s1_info;
	memcpy(&data->ipcan_dp_bearer_cdr,
	       &entry->ipcan_dp_bearer_cdr,
	       sizeof(struct ipcan_dp_bearer_cdr));
	data->sess_id = entry->sess_id;

	uint32_t addr = entry->ue_addr.u.ipv4_addr;
	DLOG(INFO) << "Adding entry for UE ip address: "
		   << ToIpv4Address(be32_t(addr)) << std::endl;
	DLOG(INFO) << "------------------------------------------------" << std::endl;
#if 0
	data->num_ul_pcc_rules = 0;
	data->num_dl_pcc_rules = 0;
#endif
	return 0;
}
/*----------------------------------------------------------------------------------*/
CommandResponse
GtpuEncap::AddSessionRecord(const bess::pb::GtpuEncapAddSessionRecordArg &arg)
{
	uint32_t teid = arg.teid();
	uint32_t ueaddr = arg.ueaddr();
	uint32_t enodeb_ip = arg.enodeb_ip();
	struct session_info sess;

	if (teid == 0)
		return CommandFailure(EINVAL, "Invalid TEID value");
	if (ueaddr == 0)
		return CommandFailure(EINVAL, "Invalid UE address");
	if (enodeb_ip == 0)
		return CommandFailure(EINVAL, "Invalid enodeB IP address");
	
	DLOG(INFO) << "Teid: " << std::hex << teid << ", ueaddr: "
		   << ToIpv4Address(be32_t(ueaddr)) << ", enodeaddr: "
		   << ToIpv4Address(be32_t(enodeb_ip)) << std::endl;

	memset(&sess, 0, sizeof(struct session_info));

	sess.ue_addr.u.ipv4_addr = ueaddr;
	sess.ul_s1_info.sgw_teid = teid;
	sess.ul_s1_info.sgw_addr.u.ipv4_addr = s1u_sgw_ip;
	sess.dl_s1_info.sgw_addr.u.ipv4_addr = s1u_sgw_ip;
	sess.ul_s1_info.enb_addr.u.ipv4_addr = enodeb_ip;
#ifdef RTE_HASH
	sess.sess_id = SESS_ID(htonl(sess.ue_addr.u.ipv4_addr), DEFAULT_BEARER);
#else
	sess.sess_id = SESS_ID(sess.ue_addr.u.ipv4_addr, DEFAULT_BEARER);
#endif
	if (dp_session_create(&sess) < 0) {
		std::cerr << "Failed to insert entry for ueaddr: "
			  << ToIpv4Address(be32_t(ueaddr)) << std::endl;
		return CommandFailure(ENOMEM, "Failed to insert session record");
	}
	return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
CommandResponse
GtpuEncap::RemoveSessionRecord(const bess::pb::GtpuEncapRemoveSessionRecordArg &arg)
{
	uint32_t ip = arg.ueaddr();
	uint64_t key;

	if (ip == 0)
		return CommandFailure(EINVAL, "Invalid UE address");

	DLOG(INFO) << "IP Address: " << ToIpv4Address(be32_t(ip)) << std::endl;
#ifdef RTE_HASH
	key = SESS_ID(htonl(ip), DEFAULT_BEARER);
#else
	key = SESS_ID(ip, DEFAULT_BEARER);
#endif

#ifndef RTE_HASH
	/* retrieve session info */
	std::pair<uint64_t, uint64_t> *value = session_map.Find(/*htonl*/(key));
	struct session_info *data = (value == NULL) ? (struct session_info *)value :
		(struct session_info *)value->second;

	if (data == NULL)
		return CommandFailure(EINVAL, "The given address does not exist");

	/* free session_info */
	rte_free(data);

	/* now remove the record */
	if (session_map.Remove(key) == false)
		return CommandFailure(EINVAL, "Failed to remove UE address");
#else
	struct session_info *data;
	if (rte_hash_lookup_data(session_map, &key, (void **)&data) < 0)
		return CommandFailure(EINVAL, "The given address does not exist");

	/* free session_info */
	rte_free(data);

	/* now remove the record */
	if (rte_hash_del_key(session_map, &key) < 0)
		return CommandFailure(EINVAL, "Failed to remove UE address");
#endif
	return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
CommandResponse
GtpuEncap::ShowRecords(const bess::pb::EmptyArg &)
{
	std::cerr << "Showing records now" << std::endl;
#ifndef RTE_HASH
	for (auto it = session_map.begin(); it != session_map.end(); it++) {
		uint64_t key = it->first;
		uint32_t ip = UE_ADDR(key);
		std::cerr << "IP Address: " << ToIpv4Address(be32_t(ip))
			  << ", Data: " << it->second << std::endl;
	}
#else
	uint32_t next = 0;;
	uint64_t *key;
	void *_data;
	int rc;
	do {
		rc = rte_hash_iterate(session_map, (const void **)&key, &_data, &next);
		if (rc >= 0) {
			uint32_t ip = UE_ADDR(*key);
			struct session_info *data = (struct session_info *)_data;
			std::cerr << "IP Address: " << ToIpv4Address(be32_t(ip))
				  << ", Data: " << data << std::endl;
		}
	} while (rc >= 0);
#endif
	return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
#ifdef RTE_HASH
void
GtpuEncap::ProcessBatch(Context *ctx, bess::PacketBatch *batch)
{
	int cnt = batch->cnt();
	int hits = 0;
	uint64_t key[bess::PacketBatch::kMaxBurst];
	void *key_ptr[bess::PacketBatch::kMaxBurst];
	struct session_info *data[bess::PacketBatch::kMaxBurst];
	uint64_t hit_mask = 0ULL;

	for (int i = 0; i < cnt; i++) {
		bess::Packet *p = batch->pkts()[i];
		/* assuming that this module comes right after EthernetDecap */
		/* pkt_len can be used as the length of IP datagram */
		Ipv4 *iph = p->head_data<Ipv4 *>();
		be32_t daddr = iph->dst;
		be32_t saddr = iph->src;
		DLOG(INFO) << "ip->saddr: " << ToIpv4Address(saddr)
			   << ", ip->daddr: " << ToIpv4Address(daddr)
			   << std::endl;
		key[i] = SESS_ID(daddr.raw_value(), DEFAULT_BEARER);
		key_ptr[i] = &key[i];
	}

	if ((hits = rte_hash_lookup_bulk_data(session_map,
					      (const void **)&key_ptr,
					      cnt,
					      &hit_mask,
					      (void **)data)) < 0) {
		DLOG(INFO) << "Failed to look-up" << std::endl;
		/* Since default module is sink, the packets go right in the dump */
		/* RunNextModule() sends batch to DEFAULT GATE */
		RunNextModule(ctx, batch);
		return;
	}

	DLOG(INFO) << "rte_hash_lookup_bulk_data output: (cnts: "
		   << cnt << ", hits: " << hits << ", hit_mask: " << hit_mask
		   << ")" << std::endl;

	for (int i = 0, j = 0; i < cnt && j < hits; i++) {
		bess::Packet *p = batch->pkts()[i];
		if (!ISSET_BIT(hit_mask, i)) {
			EmitPacket(ctx, p, DEFAULT_GATE);
			DLOG(INFO) << "Fetch failed for ip->daddr: "
				   << ToIpv4Address(be32_t(UE_ADDR(key[i])))
				   << std::endl;
			continue;
		}

		/* assuming that this module comes right after EthernetDecap */
		/* pkt_len can be used as the length of IP datagram */
		uint16_t pkt_len = p->total_len();
		Ipv4 *iph = p->head_data<Ipv4 *>();

		/* pre-allocate space for encaped header(s) */
		char *new_p = static_cast<char *>(p->prepend(sizeof(Udp) +
							     sizeof(struct gtpu_hdr) +
							     sizeof(Ipv4)));
		/* setting GTPU pointer */
		struct gtpu_hdr *gtph = (struct gtpu_hdr *)(new_p + sizeof(Ipv4) +
							    sizeof(Udp));

		/* copying template content */
		bess::utils::Copy(new_p, &outer_ip_template, sizeof(outer_ip_template));

		/* setting gtpu header */
		gtph->length = htons(pkt_len);
		gtph->teid = htonl(data[i]->ul_s1_info.sgw_teid);

		/* setting outer UDP header */
		Udp *udph = (Udp *)(new_p + sizeof(Ipv4));
		udph->length = (be16_t)(pkt_len + sizeof(struct gtpu_hdr) +
					sizeof(Udp));

		/* setting outer IP header */
		iph = (Ipv4 *)(new_p);
		iph->length = (be16_t)(pkt_len + sizeof(struct gtpu_hdr) +
				       sizeof(Udp) + sizeof(Ipv4));
		iph->src = (be32_t)(data[i]->ul_s1_info.sgw_addr.u.ipv4_addr);
		iph->dst = (be32_t)(data[i]->ul_s1_info.enb_addr.u.ipv4_addr);
		EmitPacket(ctx, p, FORWARD_GATE);
		/* increment hit idx */
		j++;
	}
}
#else /* !RTE_HASH */
/*----------------------------------------------------------------------------------*/
void
GtpuEncap::ProcessBatch(Context *ctx, bess::PacketBatch *batch)
{
	int cnt = batch->cnt();
	for (int i = 0; i < cnt; i++) {
		bess::Packet *p = batch->pkts()[i];
		/* assuming that this module comes right after EthernetDecap */
		/* pkt_len can be used as the length of IP datagram */
		uint16_t pkt_len = p->total_len();
		Ipv4 *iph = p->head_data<Ipv4 *>();
		be32_t daddr = iph->dst;
		be32_t saddr = iph->src;
		DLOG(INFO) << "ip->saddr: " << ToIpv4Address(saddr)
			   << ", ip->daddr: " << ToIpv4Address(daddr)
			   << std::endl;

		/* retrieve session info */
		uint64_t sess_id = SESS_ID(daddr.value(), DEFAULT_BEARER);
		std::pair<uint64_t, uint64_t> *result = session_map.Find(sess_id);
		struct session_info *data = (result == NULL) ? (struct session_info *)result :
			(struct session_info *)result->second;

		if (data == NULL) {
			DLOG(INFO) << "Could not find teid for IP address: "
				   << ToIpv4Address(daddr)
				   << std::endl;
			EmitPacket(ctx, p, DEFAULT_GATE);
			continue;
		}

		/* pre-allocate space for encaped header(s) */
		char *new_p = static_cast<char *>(p->prepend(sizeof(Udp) +
							     sizeof(struct gtpu_hdr) +
							     sizeof(Ipv4)));
		/* setting GTPU pointer */
		struct gtpu_hdr *gtph = (struct gtpu_hdr *)(new_p + sizeof(Ipv4) +
							    sizeof(Udp));

		/* copying template content */
		bess::utils::Copy(new_p, &outer_ip_template, sizeof(outer_ip_template));

		/* setting gtpu header */
		gtph->length = htons(pkt_len);
		gtph->teid = htonl(data->ul_s1_info.sgw_teid);

		/* setting outer UDP header */
		Udp *udph = (Udp *)(new_p + sizeof(Ipv4));
		udph->length = (be16_t)(pkt_len + sizeof(struct gtpu_hdr) +
					sizeof(Udp));

		/* setting outer IP header */
		iph = (Ipv4 *)(new_p);
		iph->length = (be16_t)(pkt_len + sizeof(struct gtpu_hdr) +
				       sizeof(Udp) + sizeof(Ipv4));
		iph->src = (be32_t)(data->ul_s1_info.sgw_addr.u.ipv4_addr);
		iph->dst = (be32_t)(data->ul_s1_info.enb_addr.u.ipv4_addr);
		EmitPacket(ctx, p, FORWARD_GATE);
	}
}
#endif /* RTE_HASH */
/*----------------------------------------------------------------------------------*/
void
GtpuEncap::DeInit()
{
#ifndef RTE_HASH
	for (auto it = session_map.begin(); it != session_map.end(); it++) {
		uint64_t key = it->first;
		struct session_info *data = (struct session_info *)it->second;
		if (data != NULL)
			rte_free(data);
		if (session_map.Remove(key) == false) {
			uint32_t ip = UE_ADDR(key);
			std::cerr << "Failed to remove record with UE address: "
				  << ToIpv4Address(be32_t(ip)) << std::endl;
		}
	}
#else
	uint32_t next = 0;
	uint64_t *key;
	void *_data;
	int rc;
	do {
		rc = rte_hash_iterate(session_map, (const void **)&key, &_data, &next);
		if (rc >= 0) {
			struct session_info *data = (struct session_info *)_data;
			/* now remove the record */
			if (rte_hash_del_key(session_map, key) < 0) {
				uint32_t ip = UE_ADDR(*key);
				std::cerr << "Failed to remove record with UE address: "
					  << ToIpv4Address(be32_t(ip)) << std::endl;
			}
			if (data != NULL)
				rte_free(data);
			/* resetting back to NULL */
			next = 0;
		}
	} while (rc >= 0);

	/* finally free the hash table */
	rte_hash_free(session_map);
#endif
}
/*----------------------------------------------------------------------------------*/
CommandResponse
GtpuEncap::Init(const bess::pb::GtpuEncapArg &arg) {

	s1u_sgw_ip = arg.s1u_sgw_ip();

	if (s1u_sgw_ip == 0)
		return CommandFailure(EINVAL,
				      "Invalid S1U SGW IP address!");

	InitNumSubs = arg.num_subscribers();
	if (InitNumSubs == 0)
		return CommandFailure(EINVAL,
				      "Invalid number of subscribers!");
#ifndef RTE_HASH
	session_map = bess::utils::CuckooMap<uint64_t, uint64_t>(InitNumBucket, InitNumSubs);
#else
	std::string hashtable_name = "session_map" + this->name();
	std::cerr << "Creating rte_hash: " << hashtable_name << std::endl;

	struct rte_hash_parameters session_map_params = {
		.name                   = hashtable_name.c_str(),
		.entries                = (unsigned int)InitNumSubs,
		.reserved               = 0,
		.key_len                = sizeof(uint64_t),
		.hash_func              = rte_jhash,
		.hash_func_init_val     = 0,
		.socket_id              = (int)rte_socket_id(),
		.extra_flag             = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY
	};

	session_map = rte_hash_create(&session_map_params);
	if (session_map == NULL)
		return CommandFailure(ENOMEM,
				      "Unable to create rte_hash table: %s\n",
				      "session_map");
#endif

	return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
ADD_MODULE(GtpuEncap, "gtpu_encap", "first version of gtpu encap module")
