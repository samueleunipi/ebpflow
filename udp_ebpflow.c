// creato da: Brendan Gregg
// modificato da: Alessandro Di Giorgio
// mail: a.digiorgio1@studenti.unipi.it

#include <uapi/linux/ptrace.h>
#include <net/sock.h>
#include <bcc/proto.h>
#include <linux/pid_namespace.h>


// Creating hash table with <key: pid, value: struct sock*>
BPF_HASH(currsock, u32, struct sock *);


// ----- ----- USER-KERNEL DATA ----- ----- //
/*
 * proto flag: 
 *    - 601  for tcp client
 *    - 602  for tcp server
 *    - 1701 for upd listen
 *    - 1702 for udp send  
 */
struct ipv4_data_t {
    u32 pid;
    u32 uid;
    u32 gid;
    u16 proto;
    u16 loc_port;
    u16 dst_port;
    u16 ip;
    u32 saddr;
    u32 daddr;
    char task[TASK_COMM_LEN];
};
BPF_PERF_OUTPUT(ipv4_events);

struct ipv6_data_t {
    u32 pid;
    u32 uid;
    u32 gid;
    u16 proto;
    u16 loc_port;
    u16 dst_port;
    u16 ip;
    unsigned __int128 saddr;
    unsigned __int128 daddr;
    char task[TASK_COMM_LEN];
};
BPF_PERF_OUTPUT(ipv6_events);


/* ******************************************* */
/* ******************************************* */

/*
 * Initializes hash entries, needs to be attached
 * to 'udp_sendmsg' function on entry (BPF_PROBE_ENTRY flag)
 * ARGS: 
 *      ctx - ebpf context
 *      sk  - socket passed to the 'udp_sendmsg'
 */
int trace_send_entry(struct pt_regs* ctx, struct sock* sk){
    u32 pid = bpf_get_current_pid_tgid();
    currsock.update(&pid, &sk);
    return 0;
}

/*
 * Handles the termination of send events. If connect
 * succed data is collected, passed to user level and
 * the entry (created in trace_connect_entry) removed from the table
 */
static int trace_send_return(struct pt_regs *ctx, short ipver){
    // udp_sendmsg return value
    int ret = PT_REGS_RC(ctx);

    // Checking if entry is present
    u32 pid = bpf_get_current_pid_tgid();
    struct sock **skpp;
    skpp = currsock.lookup(&pid);
    if (skpp == NULL || ret == -1) {
        return 0;   // missed entry
    }

    // User id and group id
    u64 guid = bpf_get_current_uid_gid();
    u32 uid = guid & 0xFFFFFFFF;
    u32 gid = (guid >> 32) & 0xFFFFFFFF;
    
    // Ports
    struct sock *skp = *skpp;
    u16 dst_port = skp->__sk_common.skc_dport;
    dst_port = ntohs(dst_port);
    u16 loc_port = skp->__sk_common.skc_num;
    loc_port = ntohs(loc_port);

    if (ipver == 4) {
        struct ipv4_data_t data4 = {.pid = pid, .ip = ipver};
        data4.saddr = skp->__sk_common.skc_rcv_saddr;
        data4.daddr = skp->__sk_common.skc_daddr;
        data4.dst_port = dst_port;
        data4.loc_port = loc_port;
        data4.uid = uid;
        data4.gid = gid;
        bpf_get_current_comm(&data4.task, sizeof(data4.task));
        ipv4_events.perf_submit(ctx, &data4, sizeof(data4)); //invio dati allo spazio utente
    } else if (ipver == 6) {
        struct ipv6_data_t data6 = {.pid = pid, .ip = ipver};
        bpf_probe_read(&data6.saddr, sizeof(data6.saddr),
            skp->__sk_common.skc_v6_rcv_saddr.in6_u.u6_addr32);
        bpf_probe_read(&data6.daddr, sizeof(data6.daddr),
            skp->__sk_common.skc_v6_daddr.in6_u.u6_addr32);
        data6.dst_port = dst_port;
        data6.loc_port = loc_port;
        data6.uid = uid;
        data6.gid = gid;
        bpf_get_current_comm(&data6.task, sizeof(data6.task));
        ipv6_events.perf_submit(ctx, &data6, sizeof(data6));
    }

    currsock.delete(&pid); //elimino entry con pid *pid

    return 0;
}

/*
 * Discriminate ip version for 'udp_send' returns event. Ipv4 and Ipv6 
 * can be discriminated by attaching this functions respectively to udp_sendmsg
 * and udpv6_sendmsg (BPF_PROBE_RETURN flag)
 */
int trace_send_v4_return(struct pt_regs *ctx) {
    return trace_send_return(ctx, 4);
}
int trace_send_v6_return(struct pt_regs *ctx) {
    return trace_send_return(ctx, 6);
}


/* ******************************************* */
/* ******************************************* */


/*
 * Handles the termination of receive events. If connect
 * succed data is collected, passed to user level and
 * the entry (created in trace_connect_entry) removed from the table
 */
static int trace_receive(struct pt_regs *ctx, struct sock *sk, short ipver){
    u32 pid = bpf_get_current_pid_tgid();

    u64 guid = bpf_get_current_uid_gid();
    u32 uid = guid & 0xFFFFFFFF;
    u32 gid = (guid >> 32) & 0xFFFFFFFF;

    int ret = PT_REGS_RC(ctx);
    if (sk == NULL || ret == -1)
        return 0;
   
     // pull in details
    u16 loc_port = sk->__sk_common.skc_num;
    loc_port = ntohs(loc_port);
    u16 dst_port = sk->__sk_common.skc_dport;
    dst_port = ntohs(dst_port);

    u16 family = 0;
    bpf_probe_read(&family, sizeof(family), &sk->__sk_common.skc_family);
    
    if(family != AF_INET && family != AF_INET6) return 0;

    if (ipver == 4) {
        struct ipv4_data_t data4 = {.pid = pid, .ip = ipver};
        bpf_probe_read(&data4.saddr, sizeof(data4.saddr), &sk->__sk_common.skc_rcv_saddr);
	bpf_probe_read(&data4.daddr, sizeof(data4.daddr), &sk->__sk_common.skc_daddr);
	bpf_probe_read(&data4.dst_port, sizeof(data4.dst_port), &dst_port);
	bpf_probe_read(&data4.loc_port, sizeof(data4.loc_port), &loc_port);
	bpf_probe_read(&data4.uid, sizeof(data4.uid), &uid);
	bpf_probe_read(&data4.uid, sizeof(data4.uid), &gid);
	
        bpf_get_current_comm(&data4.task, sizeof(data4.task));
        ipv4_events.perf_submit(ctx, &data4, sizeof(data4));
    } else if (ipver == 6) {
        struct ipv6_data_t data6 = {.pid = pid, .ip = ipver};
        bpf_probe_read(&data6.saddr, sizeof(data6.saddr), &sk->__sk_common.skc_rcv_saddr);
	bpf_probe_read(&data6.daddr, sizeof(data6.daddr), &sk->__sk_common.skc_daddr);
	bpf_probe_read(&data6.dst_port, sizeof(data6.dst_port), &dst_port);
	bpf_probe_read(&data6.loc_port, sizeof(data6.loc_port), &loc_port);
	bpf_probe_read(&data6.uid, sizeof(data6.uid), &uid);
	bpf_probe_read(&data6.gid, sizeof(data6.gid), &gid);
	
        bpf_get_current_comm(&data6.task, sizeof(data6.task));
        ipv6_events.perf_submit(ctx, &data6, sizeof(data6));
    }
    
    return 0;
}

/*
 * Discriminate ip version for 'udp_receive' returns event. Ipv4 and Ipv6 
 * can be discriminated by attaching this functions respectively to udp_recvmsg
 * and udpv6_recvmsg (BPF_PROBE_RETURN flag)
 */
int trace_receive_v4(struct pt_regs *ctx, struct sock *sk) {
    return trace_receive(ctx, sk, 4);
}
int trace_receive_v6(struct pt_regs *ctx, struct sock *sk) {
    return trace_receive(ctx, sk, 6);
}
