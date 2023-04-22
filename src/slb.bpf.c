#include "vmlinux.h"
#include "slb.h"
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>

const static __u32 map_flags = BPF_ANY;
const static __u32 FIXED_INDEX = 0;

struct conntrack_entry {
    __u32 ip;
    __u16 port;
} __attribute__((packed));
typedef struct conntrack_entry ce;


const volatile __u32 NUM_BACKENDS  = 2;

// there is something wrong with a direct enum
const volatile __u32 cur_lb_alg = 3;

const volatile __u32 local_ip = 0;

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, MAX_BACKEND);
    __type(key, __u32);
    __type(value, struct host_meta);
} backends_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct host_meta);
} vip_map SEC(".maps");


// client ip:port -> the corresponding backend
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, BACKEND_MAP_SIZE);
    __type(key, ce);
    __type(value, struct host_meta);
} back_map SEC(".maps");


__attribute__((always_inline))
static void print_mac(char *prefix ,unsigned char mac[ETH_ALEN]){
    bpf_printk("%s %02x:%02x:%02x:%02x:%02x:%02x",
        prefix,mac[0],mac[1],mac[2],
        mac[3],mac[4],mac[5]
    );
}
// __attribute__((always_inline))
// static int gen_mac(struct xdp_md *ctx, struct ethhdr *eth ,struct iphdr *iph,
//                 unsigned char n_s[ETH_ALEN],unsigned char n_d[ETH_ALEN]){  
//     // https://nakryiko.com/posts/bpf-tips-printk/ not supported yet
//     // bpf_printk("origin: 0x%pM   to 0x%pM  \n \
//     // now 0x%pM   to 0x%pM   ",
//     // eth->h_source,eth->h_dest,
//     // n_s,n_d);

//     // not enough param number in one line
//     bpf_printk("%u,origin- %02x:%02x:%02x:%02x:%02x:%02x",local_ip,
//         eth->h_source[0],eth->h_source[1],eth->h_source[2],
//         eth->h_source[3],eth->h_source[4],eth->h_source[5]
//     );
//     bpf_printk("%u,to----- %02x:%02x:%02x:%02x:%02x:%02x",local_ip,
//         eth->h_dest[0],eth->h_dest[1],eth->h_dest[2],
//         eth->h_dest[3],eth->h_dest[4],eth->h_dest[5]
//     );
//     bpf_printk("%u,now---- %02x:%02x:%02x:%02x:%02x:%02x",local_ip,
//         n_s[0],n_s[1],n_s[2],n_s[3],n_s[4],n_s[5]
//     );
//     bpf_printk("%u,to----- %02x:%02x:%02x:%02x:%02x:%02x",local_ip,
//         n_d[0],n_d[1],n_d[2],n_d[3],n_d[4],n_d[5]
//     );

//     memcpy(eth->h_source, n_s, ETH_ALEN);
//     memcpy(eth->h_dest, n_d, ETH_ALEN);
//     return XDP_TX;
// }

__attribute__((always_inline))
static int gen_mac(struct xdp_md *ctx, struct ethhdr *eth ,struct iphdr *iph,
                    __u32 ipv4_src, __u32 ipv4_dst){
    struct bpf_fib_lookup fib_params;
    memset(&fib_params, 0, sizeof(fib_params));

	fib_params.family	= AF_INET;
    fib_params.tos		= iph->tos;
    fib_params.l4_protocol	= iph->protocol;
    fib_params.sport	= 0;
    fib_params.dport	= 0;
    fib_params.tot_len	= bpf_ntohs(iph->tot_len);
    fib_params.ipv4_src	= ipv4_src;
    fib_params.ipv4_dst	= ipv4_dst;
    fib_params.ifindex = ctx->ingress_ifindex;

    bpf_printk("%u,Look up from %u|%pI4n to %u|%pI4n",local_ip,
        ipv4_src,&ipv4_src,ipv4_dst,&ipv4_dst);
    int action = XDP_PASS;
    int rc = bpf_fib_lookup(ctx, &fib_params, sizeof(fib_params), 0);
    bpf_printk("%u,origin- %02x:%02x:%02x:%02x:%02x:%02x",local_ip,
        eth->h_source[0],eth->h_source[1],eth->h_source[2],
        eth->h_source[3],eth->h_source[4],eth->h_source[5]
    );
    bpf_printk("%u,to----- %02x:%02x:%02x:%02x:%02x:%02x",local_ip,
        eth->h_dest[0],eth->h_dest[1],eth->h_dest[2],
        eth->h_dest[3],eth->h_dest[4],eth->h_dest[5]
    );
    switch (rc) {
        case BPF_FIB_LKUP_RET_SUCCESS:         /* lookup successful */
            memcpy(eth->h_dest, fib_params.dmac, ETH_ALEN);
            memcpy(eth->h_source, fib_params.smac, ETH_ALEN);
            action = XDP_TX;
            bpf_printk("%u,BPF_FIB_LKUP_RET_SUCCESS: %u, TX",local_ip,rc);
            break;
        case BPF_FIB_LKUP_RET_BLACKHOLE:    /* dest is blackholed; can be dropped */
        case BPF_FIB_LKUP_RET_UNREACHABLE:  /* dest is unreachable; can be dropped */
        case BPF_FIB_LKUP_RET_PROHIBIT:     /* dest not allowed; can be dropped */
            action = XDP_DROP;
            bpf_printk("%u,BPF_FIB_LKUP_RET_UNREACHABLE: %u, DROP",local_ip,rc);
            break;
        case BPF_FIB_LKUP_RET_NOT_FWDED:    /* packet is not forwarded */
        case BPF_FIB_LKUP_RET_FWD_DISABLED: /* fwding is not enabled on ingress */
        case BPF_FIB_LKUP_RET_UNSUPP_LWT:   /* fwd requires encapsulation */
        case BPF_FIB_LKUP_RET_NO_NEIGH:     /* no neighbor entry for nh */
        case BPF_FIB_LKUP_RET_FRAG_NEEDED:  /* fragmentation required to fwd */
            bpf_printk("%u,BPF_FIB_LKUP_RET_NOT_FWDED: %u, PASS",local_ip,rc);
            break;
	}
    bpf_printk("%u,now---- %02x:%02x:%02x:%02x:%02x:%02x",local_ip,
        eth->h_source[0],eth->h_source[1],eth->h_source[2],
        eth->h_source[3],eth->h_source[4],eth->h_source[5]
    );
    bpf_printk("%u,to----- %02x:%02x:%02x:%02x:%02x:%02x",local_ip,
        eth->h_dest[0],eth->h_dest[1],eth->h_dest[2],
        eth->h_dest[3],eth->h_dest[4],eth->h_dest[5]
    );
    return action;
}


// todo implement different load balancing algorithm
__attribute__((always_inline))
static struct host_meta *lb_hash(ce *nat_key){
    // with hash, we dobn't need to sync session amongst slb intances
    __u32 hash = ((nat_key->ip >> 7) & nat_key->port >> 3);
    bpf_printk("%u,LB hash %u",local_ip,hash);
    __u32 backend_idx = hash % NUM_BACKENDS;
    return bpf_map_lookup_elem(&backends_map, &backend_idx);
}

__attribute__((always_inline))
static struct host_meta *lb_rr(){
    static __u32 count = 0;
    __u32 backend_idx = count++ % NUM_BACKENDS;
    bpf_printk("%u,LB rr_idx %u",local_ip,backend_idx);
    return bpf_map_lookup_elem(&backends_map, &backend_idx);
}

__attribute__((always_inline)) 
static struct host_meta *lb_rand(){
    __u32 backend_idx = bpf_get_prandom_u32() % NUM_BACKENDS;
    bpf_printk("%u,LB rand_idx %u",local_ip,backend_idx);
    return bpf_map_lookup_elem(&backends_map, &backend_idx);
}

__attribute__((always_inline)) 
static struct host_meta *get_backend(enum LB_ALG alg,ce *nat_key){
    switch (alg){
        case lb_n_hash:
            return lb_hash(nat_key);
        case lb_round_robin:
            return lb_rr();   
        case lb_random: 
        default:
            return lb_rand();
    }
}


SEC("xdp")
int xdp_lb(struct xdp_md *ctx){
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;
    bpf_printk("%u,Got a packet",local_ip);
    struct ethhdr *eth = data;
    if (data + sizeof(struct ethhdr) > data_end)
        return XDP_PASS;

    if (eth->h_proto != bpf_htons(ETH_P_IP)){
        bpf_printk("%u,Not IPV4, pass",local_ip);
        return XDP_PASS;
    }

    struct iphdr *iph = data + sizeof(struct ethhdr);
    if (data + sizeof(struct ethhdr) + sizeof(struct iphdr) > data_end)
        return XDP_PASS;

    // u8,so no big or little edian
    if (iph->protocol != IPPROTO_TCP){
        bpf_printk("%u,Not TCP, pass",local_ip);
        return XDP_PASS;
    }

    struct tcphdr *tcph = data + sizeof(struct ethhdr) + sizeof(struct iphdr);
    if (data + sizeof(struct ethhdr) + sizeof(struct iphdr) + sizeof(struct tcphdr) > data_end)
        return XDP_PASS;

    //  __u32 *local_ip_addr = bpf_map_lookup_elem(&local_ip_map, &FIXED_INDEX);
    // if((!local_ip_addr)){
    //     bpf_printk("No local_ip, pass");
    //     return XDP_PASS;
    // }
    // __u32 local_ip = *local_ip_addr;
    if(iph->daddr == local_ip){
        // process as real server
        bpf_printk("%u,Process a packet of tuple\n \
        from %u|%pI4n:%u|%u to %u|%pI4n:%u|%u,\n \
        local %u|%pI4n",local_ip, 
        iph->saddr,&(iph->saddr),tcph->source,bpf_ntohs(tcph->source),
        iph->daddr,&(iph->daddr),tcph->dest,bpf_ntohs(tcph->dest),
        local_ip, &local_ip);
        return XDP_PASS;
    }

    __u16 tcp_len = bpf_ntohs(iph->tot_len) - (iph->ihl << 2);

    struct host_meta *vip = bpf_map_lookup_elem(&vip_map, &FIXED_INDEX);
    if((!vip)){
        // have to check explicitly
        bpf_printk("%u,No vip, pass",local_ip);
        return XDP_PASS;
    }
    bpf_printk("%u,Got a TCP packet of tuple \n \
            from %u|%pI4:%u|%u to %u|%pI4:%u|%u, \n \
            vip.ip_int: %u|%pI4 ",local_ip,
    iph->saddr,&(iph->saddr),tcph->source,bpf_ntohs(tcph->source),
    iph->daddr,&(iph->daddr),tcph->dest, bpf_ntohs(tcph->dest),
    vip->ip_int,&(vip->ip_int));
    if (tcp_len > TCP_MAX_BITS){
        bpf_printk("Tcp_len %u larger than max , drop",tcp_len);
        return XDP_DROP;
    }

    int action = XDP_PASS;
    if (iph->daddr == vip->ip_int){
        if(tcph->dest != vip->port){
            bpf_printk("%u,No such port %u , drop",local_ip,bpf_ntohs(tcph->dest));
            return XDP_DROP;
        }
        // Choose a backend server to send the request to; 
        // within a lifetime of tcp conn, backend must be the same
        ce nat_key = {
            // keep the original net edian
            .ip = iph->saddr,
            .port = tcph->source
        };
        struct host_meta *rs = bpf_map_lookup_elem(&back_map, &nat_key);
        if (rs == NULL){
            rs = get_backend(cur_lb_alg,&nat_key);
            if(!rs){
                bpf_printk("%u,No rs, pass",local_ip);
                return XDP_PASS;
            }
            bpf_map_update_elem(&back_map, &nat_key, rs, map_flags); 
        }
        if(rs->ip_int == local_ip){
            bpf_printk("%u,Picked this rs, pass",local_ip);
            return XDP_PASS;
        }
        // action = gen_mac(ctx,eth,iph,vip->mac_addr,rs->mac_addr);
        action = gen_mac(ctx,eth,iph,local_ip,rs->ip_int);
        bpf_printk("%u,Ingress a nat packet of tuple\n \
        from %u|%pI4n:%u|%u to %u|%pI4n:%u|%u,",local_ip, 
        iph->saddr,&(iph->saddr),tcph->source,bpf_ntohs(tcph->source),
        iph->daddr,&(iph->daddr),tcph->dest,bpf_ntohs(tcph->dest));
    }else{
        bpf_printk("%u,No such ip %pI4n , drop",local_ip,&iph->daddr);
        return XDP_DROP;
    }
    return action;
}

char _license[] SEC("license") = "GPL";