/*
 *      $Id$
 */
/************************************************************************
*									*
*			     Copyright (C)  2002			*
*				Internet2				*
*			     All Rights Reserved			*
*									*
************************************************************************/
/*
 *	File:		owping.c
 *
 *	Authors:	Jeff Boote
 *                      Anatoly Karp
 *			Internet2
 *
 *	Date:		Thu Apr 25 12:22:31  2002
 *
 *	Description:	
 *
 *	Initial implementation of owping commandline application. This
 *	application will measure active one-way udp latencies.
 */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <netdb.h>

#include <I2util/util.h>
#include <owamp/owamp.h>
#include <owamp/conndata.h>
#include <owamp/access.h>

#include "./owpingP.h"

/*
 * The owping context
 */
static	ow_ping_trec	ping_ctx;
static I2ErrHandle	eh;

#define OWP_TMPFILE "/tmp/owamp.XXXXXX"

#ifdef	NOT
static int
OWPingErrFunc(
	void		*app_data,
	OWPErrSeverity	severity	__attribute__((unused)),
	OWPErrType	etype,
	const char	*errmsg
)
{
	ow_ping_t		pctx = (ow_ping_t)app_data;

	/*
	 * If not debugging - only print messages of warning or worse.
	 * (unless of course verbose is specified...
	 */
#ifdef	NDEBUG
	if(!pctx->opt.verbose && (severity > OWPErrWARNING))
		return 0;
#endif

	I2ErrLogP(pctx->eh,etype,errmsg);

	return 0;
}
#endif
	
/*
 * Library initialization structure;
 */
static	OWPInitializeConfigRec	OWPCfg = {{
	/* tm_out.tv_sec		*/	0,
	/* tm_out.tv_usec		*/	0},
	/* eh				*/	NULL,
	/* get_aes_key			*/	owp_get_aes_key,
	/* check_control_func		*/	NULL,
	/* check_test_func		*/	NULL,
	/* rand_type                    */      I2RAND_DEV,
	/* rand_data                    */      NULL
};

static void
print_conn_args()
{
	fprintf(stderr, "%s\n\n%s\n%s\n%s\n%s\n",
		"              [Connection Args]",
"   -A authmode    requested modes: [A]uthenticated, [E]ncrypted, [O]pen",
"   -k keyfile     AES keyfile to use with Authenticated/Encrypted modes",
"   -u username    username to use with Authenticated/Encrypted modes",
"   -S srcaddr     use this as a local address for control connection and tests");
}

static void
print_test_args()
{
	fprintf(stderr, "%s\n\n%s\n%s\n%s\n%s\n%s\n%s\n",
		"              [Test Args]",
"   -f | -F file   perform one-way test from testhost [and save results to file]",
"   -t | -T file   perform one-way test to testhost [and save results to file]",
"   -c count       number of test packets",
"   -i wait        mean average time between packets (seconds)",
"   -L timeout     maximum time to wait for a packet before declaring it lost",
"   -s padding     size of the padding added to each packet (bytes)");
}

static void
print_output_args()
{
	fprintf(stderr, "%s\n\n%s\n%s\n%s\n%s\n",
		"              [Output Args]",
		"   -h             print this message and exit",
		"   -Q             run the test and exit without reporting statistics",
		"   [-v | -V]      print out individual delays, or full timestamps",
		"   -a alpha       report an additional percentile level for the delays"
		);
}

static void
usage(const char *progname, const char *msg)
{
	if(msg) fprintf(stderr, "%s: %s\n", progname, msg);
	if (!strcmp(progname, "owping")) {
		fprintf(stderr,
			"usage: %s %s\n%s\n", 
			progname, "[arguments] testaddr [servaddr]",
			"[arguments] are as follows: "
			);
		fprintf(stderr, "\n");
		print_conn_args();
		
		fprintf(stderr, "\n");
		print_test_args();
		
		fprintf(stderr, "\n");
		print_output_args();
		
	} else if (!strcmp(progname, "owstats")) {
		fprintf(stderr,
			"usage: %s %s\n%s\n",
			progname, "[arguments] sessionfile",
			"[arguments] are as follows: "
			);
		fprintf(stderr, "\n");
		print_output_args();
	} else if (!strcmp(progname, "owfetch")) {
		fprintf(stderr,
			"usage: %s %s\n%s\n",
			progname, "[arguments] servaddr [SID savefile]+",
			"[arguments] are as follows: "
			);
		fprintf(stderr, "\n");
		print_conn_args();
		fprintf(stderr, "\n");
		print_output_args();
	}

	return;
}

static void
FailSession(
	OWPControl	control_handle	__attribute__((unused))
	   )
{
	/*
	 * Session denied - report error, close connection, and exit.
	 */
	I2ErrLog(eh, "Session Failed!");
	fflush(stderr);

	/* TODO: determine "reason" for denial and report */
	(void)OWPControlClose(ping_ctx.cntrl);
	exit(1);
}

#define THOUSAND 1000.0

/* Width of Fetch receiver window. */
#define OWP_WIN_WIDTH   64

#define OWP_MAX_N           100  /* N-reordering statistics parameter */
#define OWP_LOOP(x)         ((x) >= 0? (x): (x) + OWP_MAX_N)

/*
** Generic state to be maintained by client during Fetch.
*/
typedef struct fetch_state {
	FILE*        fp;               /* stream to report records           */
	OWPDataRec window[OWP_WIN_WIDTH]; /* window of read records    */
	OWPDataRec last_out; /* last processed record            */
	int          cur_win_size;     /* number of records in the window    */
	double       tmin;             /* min delay                          */
	double       tmax;             /* max delay                          */
	u_int32_t    num_received;     /* number of good received packets    */
	u_int32_t    dup_packets;      /* number of duplicate packets        */
	int          order_disrupted;  /* flag                               */
	u_int32_t    max_seqno;        /* max sequence number seen           */
	u_int32_t    *buckets;         /* array of buckets of counts         */
	char         *from;            /* Endpoints in printable format      */
	char         *to;
	u_int32_t    count_out;        /* number of printed packets          */

	/* N-reodering state variables. */
	u_int32_t        m[OWP_MAX_N];       /* We have m[j-1] == number of
						j-reordered packets.         */
        u_int32_t        ring[OWP_MAX_N];    /* Last sequence numbers seen.  */
        u_int32_t        r;                  /* Ring pointer for next write. */
        u_int32_t        l;                  /* Number of seq numbers read.  */

} fetch_state, *fetch_state_ptr;

#define OWP_CMP(a,b) ((a) < (b))? -1 : (((a) == (b))? 0 : 1)
#define OWP_MIN(a,b) ((a) < (b))? (a) : (b)

/*
** The function returns -1. 0 or 1 if the first record's sequence
** number is respectively less than, equal to, or greater than that 
** of the second.
*/
int
owp_seqno_cmp(OWPDataRecPtr a, OWPDataRecPtr b)
{
	assert(a); assert(b);
	return OWP_CMP(a->seq_no, b->seq_no);
}

/*
** Find the right spot in the window to insert the new record <rec>
** Return max {i| 0 <= i <= cur_win_size-1 and <rec> is later than the i_th
** record in the state window}, or -1 if no such index is found.
*/
int
look_for_spot(fetch_state_ptr state,
	      OWPDataRecPtr rec)
{
	int i;
	assert(state->cur_win_size);

	for (i = state->cur_win_size - 1; i >= 0; i--) {
		if (owp_seqno_cmp(&state->window[i], rec) < 0)
			return i;
	}
	
	return -1;
}

double
owp_bits2prec(int nbits)
{
	return (nbits >= 32)? 1.0/(1 << (nbits - 32)) 
		: (double)(1 << (32 - nbits));
}

/*
** Generic function to output timestamp record <rec> in given format
** as encoded in <state>.
*/
void
owp_record_out(fetch_state_ptr state, OWPDataRecPtr rec)
{
	double delay;

	assert(rec);
	assert(state);

	if (!ping_ctx.opt.records)
		return;

	assert(state->fp);

	if (!(state->count_out++ & 31))
	       fprintf(state->fp,"--- owping test session from %s to %s ---\n",
		       (state->from)? state->from : "***", 
		       (state->to)?  state->to : "***");

	delay = owp_delay(&rec->send, &rec->recv);
	if (ping_ctx.opt.full) {
		if (!OWPIsLostRecord(rec))
			fprintf(state->fp, 
	 "#%-10u send=%8X:%-8X %u%c     recv=%8X:%-8X %u%c\n",
				rec->seq_no, rec->send.sec, 
				rec->send.frac_sec, 
				rec->send.prec, (rec->send.sync)? 'S' : 'U', 
				rec->recv.sec, rec->recv.frac_sec, 
				rec->recv.prec, (rec->recv.sync)? 'S' : 'U');
		else
			fprintf(state->fp, 
				"#%-10u send=%8X:%-8X %u%c     *LOST*\n",
				rec->seq_no, rec->send.sec, 
				rec->send.frac_sec, 
				rec->send.prec, 
				(rec->send.sync)? 'S' : 'U');
		return;
	}

	if (!OWPIsLostRecord(rec)) {
		if (rec->send.sync && rec->recv.sync) {
			double prec = owp_bits2prec(rec->send.prec) 
				+ owp_bits2prec(rec->recv.prec);
			fprintf(state->fp, 
	       "seq_no=%-10u delay=%.3f ms       (sync, precision %.3f ms)\n", 
				rec->seq_no, delay*THOUSAND, 
				prec*THOUSAND);
		
			return;
		}
		fprintf(state->fp, "seq_no=%u delay=%.3f ms (unsync)\n",
			rec->seq_no, delay*THOUSAND);
		return;
	}
	fprintf(state->fp, "seq_no=%-10u *LOST*\n", rec->seq_no);
}

#define OWP_MAX_BUCKET  (OWP_NUM_LOW + OWP_NUM_MID + OWP_NUM_HIGH - 1)

#define OWP_NUM_LOW         50000
#define OWP_NUM_MID         100000
#define OWP_NUM_HIGH        49900

#define OWP_CUTOFF_A        (double)(-50.0)
#define OWP_CUTOFF_B        (double)0.0
#define OWP_CUTOFF_C        (double)0.1
#define OWP_CUTOFF_D        (double)50.0

const double mesh_low = (OWP_CUTOFF_B - OWP_CUTOFF_A)/OWP_NUM_LOW;
const double mesh_mid = (OWP_CUTOFF_C - OWP_CUTOFF_B)/OWP_NUM_MID;
const double mesh_high = (OWP_CUTOFF_D - OWP_CUTOFF_C)/OWP_NUM_HIGH;

int
owp_bucket(double delay)
{
	if (delay < OWP_CUTOFF_A)
		return 0;

	if (delay < OWP_CUTOFF_B)
		return OWP_NUM_LOW + (int)(delay/mesh_low);

	if (delay < OWP_CUTOFF_C)
		return OWP_NUM_LOW +  (int)(delay/mesh_mid);

	if (delay < OWP_CUTOFF_D)
		return OWP_NUM_LOW + OWP_NUM_MID 
			+ (int)((delay - OWP_CUTOFF_C)/mesh_high);
	
	return OWP_MAX_BUCKET;
}

void
owp_update_stats(fetch_state_ptr state, OWPDataRecPtr rec) {
	double delay;  
	int bucket;

	assert(state); assert(rec);

	if (state->num_received++ && !owp_seqno_cmp(rec, &state->last_out)){
		state->dup_packets++;
		return;
	}

	delay =  owp_delay(&rec->send, &rec->recv);
	bucket = owp_bucket(delay);
	
	assert((0 <= bucket) && (bucket <= OWP_MAX_BUCKET));
	state->buckets[bucket]++;

	if (delay < state->tmin)
		state->tmin = delay;
	if (delay > state->tmax)
		state->tmax = delay;
	
	if (rec->seq_no > state->max_seqno)
		state->max_seqno = rec->seq_no;

	memcpy(&state->last_out, rec, sizeof(*rec));
}

/*
** Given a number <alpha> in [0, 1], compute
** min {x: F(x) >= alpha}
** where F is the empirical distribution function (in our case,
** with a fuzz factor due to use of buckets.
*/
double
owp_get_percentile(fetch_state_ptr state, double alpha)
{
	int i;
	double sum = 0;
	u_int32_t unique = state->num_received - state->dup_packets;

	assert((0.0 <= alpha) && (alpha <= 1.0));
	
	for (i = 0; (i <= OWP_MAX_BUCKET) && (sum < alpha*unique); i++)
		sum += state->buckets[i];

	if (i <= OWP_NUM_LOW)
		return OWP_CUTOFF_A + i*mesh_low;
	if (i <= OWP_NUM_LOW + OWP_NUM_MID)
		return OWP_CUTOFF_B + (i - OWP_NUM_LOW)*mesh_mid;
	return OWP_CUTOFF_C + (i - (OWP_NUM_LOW+OWP_NUM_MID))*mesh_high;

	return 0.0;
}

/* True if the first timestamp is earlier than the second. */
#define OWP_EARLIER_THAN(a, b)                                \
( ((a).sec < (b).sec)                                         \
  || ( ((a).sec == (b).sec)                                   \
       && ((a).frac_sec < (b).frac_sec)                       \
     )                                                        \
) 

#define OWP_OUT_OF_ORDER(new, last_out)                       \
(                                                             \
((new)->seq_no < (last_out)->seq_no)                          \
|| (                                                          \
      ((new)->seq_no == (last_out)->seq_no)                   \
      && OWP_EARLIER_THAN(((new)->recv), ((last_out)->recv))  \
   )                                                          \
)

/*
** Processs a single record, updating statistics and internal state.
** Return 0 on success, or -1 on failure, 1 to stop parsing data.
*/
int
do_single_record(void *calldata, OWPDataRecPtr rec) 
{
	int i;
	fetch_state_ptr state = (fetch_state_ptr)calldata;
	u_int32_t j;

	assert(state);

	owp_record_out(state, rec); /* Output is done in all cases. */

	if (OWPIsLostRecord(rec))
		return 0;       /* May do something better later. */

	/* If ordering is important - handle it here. */
	if (state->order_disrupted)
		return 0;
	
	/* Update N-reordering state. */
	for (j = 0; j < OWP_MIN(state->l, OWP_MAX_N); j++) { 
		 if(rec->seq_no 
		       >= state->ring[OWP_LOOP((int)(state->r - j - 1))])
			 break;
		 state->m[j]++;
	}
	state->ring[state->r] = rec->seq_no;
	state->l++;
	state->r = (state->r + 1) % OWP_MAX_N;

	if (state->cur_win_size < OWP_WIN_WIDTH){/* insert - no stats updates*/
		if (state->cur_win_size) { /* Grow window. */
			int num_records_to_move;
			i = look_for_spot(state, rec);
			num_records_to_move = state->cur_win_size - i - 1;

			/* Cut and paste if needed - then insert. */
			if (num_records_to_move) 
				memmove(&state->window[i+2], 
					&state->window[i+1], 
					num_records_to_move*sizeof(*rec));
			memcpy(&state->window[i+1], rec, sizeof(*rec)); 
		}  else /* Initialize window. */
			memmove(&state->window[0], rec, sizeof(*rec));
		state->cur_win_size++;
	} else { /* rotate - update state*/
		OWPDataRecPtr out_rec = rec;		
		if (state->num_received
		    && OWP_OUT_OF_ORDER(rec, &(state->last_out))) {
				state->order_disrupted = 1;
				/* terminate parsing */
				return 1; 
		}

		i = look_for_spot(state, rec);

		if (i != -1)
			out_rec = &state->window[0];
		owp_update_stats(state, out_rec);

		/* Update the window.*/
		if (i != -1) {  /* Shift if needed - then insert.*/
			if (i) 
				memmove(&state->window[0],
					&state->window[1], i*sizeof(*rec));
			memcpy(&state->window[i], rec, sizeof(*rec));
		} 
	}
	
	return 0;
}

/*
** Print out summary results, ping-like style. sent + dup == lost +recv.
*/
int
owp_do_summary(fetch_state_ptr state)
{
	double min = ((double)(state->tmin)) * THOUSAND;    /* msec */
	u_int32_t sent = state->max_seqno + 1;
	u_int32_t lost = state->dup_packets + sent - state->num_received; 
	double percent_lost = (100.0*(double)lost)/(double)sent;
	int j;

	assert(state); assert(state->fp);

	fprintf(state->fp, "\n--- owping statistics from %s to %s ---\n",
		       (state->from)? state->from : "***", 
		       (state->to)?  state->to : "***");
	if (state->dup_packets)
		fprintf(state->fp, 
 "%u packets transmitted, %u packets lost (%.1f%% loss), %u duplicates\n",
			sent, lost, percent_lost, state->dup_packets);
	else	
		fprintf(state->fp, 
		     "%u packets transmitted, %u packets lost (%.1f%% loss)\n",
			sent ,lost, percent_lost);

	fprintf(state->fp, "one-way delay min/median = %.3f/%.3f ms\n", 
		min, owp_get_percentile(state, 0.5)*THOUSAND);

	for (j = 0; j < OWP_MAX_N && state->m[j]; j++)
                fprintf(state->fp,
			"%d-reordering = %f%%\n", j+1, 
			100.0*state->m[j]/(state->l - j - 1));
        if (j == 0) 
		fprintf(state->fp, "no reordering\n");
        else 
		if (j < OWP_MAX_N) 
			fprintf(state->fp, "no %d-reordering\n", j + 1);
        else 
		fprintf(state->fp, 
			"only up to %d-reordering is handled\n", OWP_MAX_N);

	if ((ping_ctx.opt.percentile - 50.0) > 0.000001
	    || (ping_ctx.opt.percentile - 50.0) < -0.000001) {
		float x = ping_ctx.opt.percentile/100.0;
		fprintf(state->fp, 
			"%.2f percentile of one-way delays: %.3f ms\n",
			ping_ctx.opt.percentile,
			owp_get_percentile(state, x) * THOUSAND);
	}
	
	fprintf(state->fp, "\n");

	return 0;
}

/*
** Master output function - reads the records from the disk
** and prints them to <out> in a style specified by <type>.
** Its value is interpreted as follows:
** 0 - print out send and recv timestamsps for each record in machine-readable
** format;
** 1 - print one-way delay in msec for each record, and final summary
**     (original ping style: max/avg/min/stdev) at the end.
*/
int
do_records_all(
		FILE		*fp,
		fetch_state_ptr	state,
		char		*from,
		char		*to
		)
{
	int		i, num_buckets;
	u_int32_t	num_rec,hdr_len;

	/*
	  Initialize fields of state to keep track of.
	*/
	state->cur_win_size = 0;
	state->tmin = 9999.999;
	state->tmax = 0.0;
	state->num_received = state->dup_packets = state->max_seqno = 0;

	state->order_disrupted = 0;

	state->from = from;
	state->to = to;
	state->count_out = 0;

	/* N-reodering fields/ */
	state->r = state->l = 0;
	for (i = 0; i < OWP_MAX_N; i++) 
		state->m[i] = 0;

	num_buckets = OWP_NUM_LOW + OWP_NUM_MID + OWP_NUM_HIGH;

	state->buckets 
		= (u_int32_t *)malloc(num_buckets*sizeof(*(state->buckets)));
	if (!state->buckets) {
		I2ErrLog(eh, "FATAL: main: malloc(%d) failed: %M",num_buckets);
		exit(1);
	}
	for (i = 0; i <= OWP_MAX_BUCKET; i++)
		state->buckets[i] = 0;

	if(!(num_rec = OWPReadDataHeader(fp,&hdr_len))){
		I2ErrLog(eh, "OWPReadDataHeader:Empty file?");
		return -1;
	}
	
	if(OWPParseRecords(fp,num_rec,do_single_record,state) < OWPErrWARNING){
		I2ErrLog(eh,"OWPParseRecords():%M");
		return -1;
	}
	
	/* Stats are requested and failed to keep records sorted - redo */
	if (state->order_disrupted) {
		I2ErrLog(eh, "Severe out-of-order condition observed.");
		I2ErrLog(eh, 
	     "Producing statistics for this case is currently unimplemented.");
		return 0;
	}

	/* Incorporate remaining records left in the window. */
	for (i = 0; i < state->cur_win_size; i++)
		owp_update_stats(state, &state->window[i]);

	owp_do_summary(state);
	free(state->buckets);
	return 0;
}

/*
** Fetch a session with the given <sid> from the remote server.
** It is assumed that control connection has been opened already.
*/
void
owp_fetch_sid(
	      char *savefile,        
	      OWPControl cntrl, 
	      OWPSID sid,       
	      fetch_state_ptr statep,
	      char *local,           
	      char *remote,
	      int do_stats
	      )
{
	char		*path;
	FILE		*fp;
	u_int32_t	num_rec;
	OWPErrSeverity	rc=OWPErrOK;

	/*
	 * Prepare paths for datafiles. Unlink if not keeping data.
	 */
	if(savefile){
		path = savefile;
		if( !(fp = fopen(path,"wb+"))){
			I2ErrLog(eh,"owp_fetch_sid:fopen(%s):%M",path);
			exit(1);
		}
	}
	else{
		/*
		 * Using fd/mkstemp/fdopen to avoid race condition that
		 * would exist if we used mktemp/fopen.
		 */
		int	fd;

		path = strdup(OWP_TMPFILE);
		if(!path){
			I2ErrLog(eh,"owp_fetch_sid:strdup(%s):%M",OWP_TMPFILE);
			exit(1);
		}
		if((fd = mkstemp(path)) < 0){
			I2ErrLog(eh,"owp_fetch_sid:mkstemp(%s):%M",path);
			exit(1);
		}
		if(!(fp = fdopen(fd,"wb+"))){
			I2ErrLog(eh,"owp_fetch_sid:fdopen():%M");
			exit(1);
		}
		if (unlink(path) < 0) {
			I2ErrLog(eh,"owp_fetch_sid:unlink(%s):%M",path);
		}
		free(path);
		path = NULL;
	}


	/*
	 * Ask for complete session 
	 * TODO: In v5 this function will return interesting info about
	 * the session. For now, it just returns the number of records.
	 */
	num_rec = OWPFetchSession(cntrl,fp,0,(u_int32_t)0xFFFFFFFF,sid,&rc);
	if(!num_rec){
		if(path)
			(void)unlink(path);
		if(rc < OWPErrWARNING){
			I2ErrLog(eh,"owp_fetch_sid:OWPFetchSession error?");
			exit(1);
		}
		/*
		 * server denied request...
		 */
		I2ErrLog(eh,
		"owp_fetch_sid:Server denied request for to session data");
		return;
	}

	if (do_stats) {
		if(do_records_all(fp,statep,local,remote) < 0) {
			I2ErrLog(eh, "FATAL: do_records_all(to session)");
		}
	}
	
	if(fclose(fp) != 0) {
		I2ErrLog(eh,"fclose():%M");
	}
	return;
}

/*
** Initialize authentication and policy data (used by owping and owfetch)
*/
void
owp_set_auth(ow_ping_trec *pctx, 
	     owp_policy_data **policy, 
	     char *progname,
	     OWPContext ctx)
{
	OWPErrSeverity err_ret;

	if(pctx->opt.identity){
		/*
		 * Eventually need to modify the policy init for the
		 * client to deal with a pass-phrase instead of/ or in
		 * addition to the passwd file.
		 */
		*policy = OWPPolicyInit(ctx, NULL, NULL, pctx->opt.passwd, 
				       &err_ret);
		if (err_ret == OWPErrFATAL){
			I2ErrLog(eh, "PolicyInit failed. Exiting...");
			exit(1);
		};
	}


	/*
	 * Verify/decode auth options.
	 */
	if(pctx->opt.authmode){
		char	*s = ping_ctx.opt.authmode;
		pctx->auth_mode = 0;
		while(*s != '\0'){
			switch (toupper(*s)){
				case 'O':
				pctx->auth_mode |= OWP_MODE_OPEN;
				break;
				case 'A':
				pctx->auth_mode |= OWP_MODE_AUTHENTICATED;
				break;
				case 'E':
				pctx->auth_mode |= OWP_MODE_ENCRYPTED;
				break;
				default:
				I2ErrLogP(eh,EINVAL,"Invalid -authmode %c",*s);
				usage(progname, NULL);
				exit(1);
			}
			s++;
		}
	}else{
		/*
		 * Default to all modes.
		 * If identity not set - library will ignore A/E.
		 */
		pctx->auth_mode = OWP_MODE_OPEN|OWP_MODE_AUTHENTICATED|
							OWP_MODE_ENCRYPTED;
	}
}

/*
 * TODO: Find real max padding sizes based upon size of headers
 */
#define	MAX_PADDING_SIZE	65000

int
main(
	int	argc,
	char	**argv
) {
	char			*progname;
	OWPErrSeverity		err_ret = OWPErrOK;
	I2LogImmediateAttr	ia;
	owp_policy_data		*policy;
	OWPContext		ctx;
	OWPTestSpecPoisson	test_spec;
	struct timeval		delay_tval;
	OWPSID			tosid, fromsid;
	OWPPerConnDataRec	conndata;
	OWPAcceptType		acceptval;
	OWPErrSeverity		err;
	fetch_state             state;
	FILE			*fromfp=NULL;
	OWPAddr                 local;
	char                    local_str[NI_MAXHOST], *remote;

	int			ch;
	char                    *endptr = NULL;
	char                    optstring[128];
	static char		*conn_opts = "A:S:k:u:";
	static char		*test_opts = "fF:tT:c:i:s:L:";
	static char		*out_opts = "a:vVQ";
	static char		*gen_opts = "h";
#ifndef	NDEBUG
	static char		*debug_opts = "w";
#endif

	ia.line_info = (I2NAME | I2MSG);
	ia.fp = stderr;

	progname = (progname = strrchr(argv[0], '/')) ? ++progname : *argv;

	state.fp = stdout;

	/*
	* Start an error logging session for reporing errors to the
	* standard error
	*/
	eh = I2ErrOpen(progname, I2ErrLogImmediate, &ia, NULL, NULL);
	if(! eh) {
		fprintf(stderr, "%s : Couldn't init error module\n", progname);
		exit(1);
	}
	OWPCfg.eh = eh;

	OWPCfg.tm_out.tv_sec = 60;   /* just for now */
	/*
	 * Initialize library with configuration functions.
	 */
	if( !(ping_ctx.lib_ctx = OWPContextInitialize(&OWPCfg))){
		I2ErrLog(eh, "Unable to initialize OWP library.");
		exit(1);
	}
	ctx = ping_ctx.lib_ctx;

	/* Set default options. */
	ping_ctx.opt.records = ping_ctx.opt.full = ping_ctx.opt.childwait 
            = ping_ctx.opt.from = ping_ctx.opt.to = ping_ctx.opt.quiet = False;
	ping_ctx.opt.save_from_test = ping_ctx.opt.save_to_test 
		= ping_ctx.opt.identity = ping_ctx.opt.passwd 
		= ping_ctx.opt.srcaddr = ping_ctx.opt.authmode = NULL;
	ping_ctx.opt.numPackets = 100;
	ping_ctx.opt.lossThreshold = 0;
	ping_ctx.opt.percentile = 50.0;
	ping_ctx.opt.mean_wait = (float)0.1;
	ping_ctx.opt.padding = 0;

	/* Create options strings for this program. */
	if (!strcmp(progname, "owping")) {
		strcpy(optstring, conn_opts);
		strcat(optstring, test_opts);
		strcat(optstring, out_opts);
	} else if (!strcmp(progname, "owstats")) {
		strcpy(optstring, out_opts);
	} else if (!strcmp(progname, "owfetch")) {
		strcpy(optstring, conn_opts);
		strcat(optstring, out_opts);
	}
	strcat(optstring, gen_opts);
#ifndef	NDEBUG
	strcat(optstring,debug_opts);
#endif
		
	while ((ch = getopt(argc, argv, optstring)) != -1)
             switch (ch) {
		     /* Connection options. */
             case 'A':
		     if (!(ping_ctx.opt.authmode = strdup(optarg))) {
			     I2ErrLog(eh,"malloc:%M");
			     exit(1);
		     }
                     break;
             case 'S':
		     if (!(ping_ctx.opt.srcaddr = strdup(optarg))) {
			     I2ErrLog(eh,"malloc:%M");
			     exit(1);
		     }
                     break;
             case 'u':
		     if (!(ping_ctx.opt.identity = strdup(optarg))) {
			     I2ErrLog(eh,"malloc:%M");
			     exit(1);
		     }
                     break;
	     case 'k':
		     if (!(ping_ctx.opt.passwd = strdup(optarg))) {
			     I2ErrLog(eh,"malloc:%M");
			     exit(1);
		     }
                     break;

		     /* Test options. */
  	     case 'F':
		     if (!(ping_ctx.opt.save_from_test = strdup(optarg))) {
			     I2ErrLog(eh,"malloc:%M");
			     exit(1);
		     }     
		     /* fall through */
             case 'f':
		     ping_ctx.opt.from = True;
                     break;
	     case 'T':
		     if (!(ping_ctx.opt.save_to_test = strdup(optarg))) {
			     I2ErrLog(eh,"malloc:%M");
			     exit(1);
		     }
		     /* fall through */
             case 't':
		     ping_ctx.opt.to = True;
                     break;
             case 'c':
		     ping_ctx.opt.numPackets = strtoul(optarg, &endptr, 10);
		     if (*endptr != '\0') {
			     usage(progname, 
				   "Invalid value. Positive integer expected");
			     exit(1);
		     }
                     break;
             case 'i':
		     ping_ctx.opt.mean_wait = (float)(strtod(optarg, &endptr));
		     if (*endptr != '\0') {
			     usage(progname, 
			   "Invalid value. Positive floating number expected");
			     exit(1);
		     }
                     break;
             case 's':
		     ping_ctx.opt.padding = strtoul(optarg, &endptr, 10);
		     if (*endptr != '\0') {
			     usage(progname, 
				   "Invalid value. Positive integer expected");
			     exit(1);
		     }
                     break;
             case 'L':
		     ping_ctx.opt.lossThreshold = strtoul(optarg, &endptr, 10);
		     if (*endptr != '\0') {
			     usage(progname, 
				   "Invalid value. Positive integer expected");
			     exit(1);
		     }
                     break;


		     /* Output options */
             case 'V':
		     ping_ctx.opt.full = True;
		     /* fall-through */
             case 'v':
		     ping_ctx.opt.records = True;
                     break;
             case 'Q':
		     ping_ctx.opt.quiet = True;
                     break;

             case 'a':
		     ping_ctx.opt.percentile =(float)(strtod(optarg, &endptr));
		     if ((*endptr != '\0')
			 || (ping_ctx.opt.percentile < 0.0) 
			 || (ping_ctx.opt.percentile > 100.0)){
			     usage(progname, 
	     "Invalid value. Floating number between 0.0 and 100.0 expected");
			     exit(1);
		     }
		     break;
	     case 'w':
		     ping_ctx.opt.childwait = True;
                     break;

		     /* Generic options.*/
             case 'h':
             case '?':
             default:
                     usage(progname, "");
		     exit(0);
		     /* UNREACHED */
             }
	argc -= optind;
	argv += optind;

	/*
	 * Handle 3 possible cases (owping, owfetch, owstats) one by one.
	 */
	if (!strcmp(progname, "owping")){

		if((argc < 1) || (argc > 2)){
			usage(progname, NULL);
			exit(1);
		}

		if(!ping_ctx.opt.to && !ping_ctx.opt.from)
			ping_ctx.opt.to = ping_ctx.opt.from = True;

		ping_ctx.remote_test = argv[0];
		if(argc > 1)
			ping_ctx.remote_serv = argv[1];
		else
			ping_ctx.remote_serv = ping_ctx.remote_test;

		/*
		 * This is in reality dependent upon the actual protocol used
		 * (ipv4/ipv6) - it is also dependent upon the auth mode since
		 * authentication implies 128bit block sizes.
		 */
		if(ping_ctx.opt.padding > MAX_PADDING_SIZE)
			ping_ctx.opt.padding = MAX_PADDING_SIZE;


		if ((ping_ctx.opt.percentile < 0.0) 
		    || (ping_ctx.opt.percentile > 100.0)) {
			usage(progname, "alpha must be between 0.0 and 100.0");
			exit(0);
		}

		owp_set_auth(&ping_ctx, &policy, progname, ctx); 

		
		test_spec.test_type = OWPTestPoisson;
		test_spec.npackets = ping_ctx.opt.numPackets;
		
		/*
		 * TODO: Figure out typeP...
		 */
		test_spec.typeP = 0;      /* so it's in host byte order then */
		test_spec.packet_size_padding = ping_ctx.opt.padding;
		
		/* InvLambda is mean wait time in usec */
		test_spec.InvLambda 
			= (double)1000000.0 * ping_ctx.opt.mean_wait;
		
		conndata.pipefd = -1;
		conndata.policy = policy; /* or NULL ??? */
		conndata.node = NULL;
#ifndef	NDEBUG
		conndata.childwait = ping_ctx.opt.childwait;
#endif
		
		/*
		 * Open connection to owampd.
		 */
		
		ping_ctx.cntrl = OWPControlOpen(ctx, 
			OWPAddrByNode(ctx, ping_ctx.opt.srcaddr),
			OWPAddrByNode(ctx, ping_ctx.remote_serv),
			ping_ctx.auth_mode, 
			ping_ctx.opt.identity,
			(void*)&conndata, &err_ret);
		if (!ping_ctx.cntrl){
			I2ErrLog(eh, "Unable to open control connection.");
			exit(1);
		}

		conndata.cntrl = ping_ctx.cntrl;
		
		(void)OWPGetDelay(ping_ctx.cntrl,&delay_tval);
		/*
		 * Set the loss threshold to 2 seconds longer then the
		 * rtt delay estimate. 2 is just a guess for a good number
		 * based upon how impatient a command-line user gets for
		 * results. For the results to have any statistical relevance
		 * the lossThreshold should be specified on the command
		 * line.
		 *
		 * TODO:
		 * (The lossThreshold turns into the "timeout" parameter
		 * of the actual test request in the next version of the
		 * protocol.)
		 */
		if(!ping_ctx.opt.lossThreshold){
			ping_ctx.opt.lossThreshold = delay_tval.tv_sec + 2;
		}
		conndata.lossThreshold = ping_ctx.opt.lossThreshold;

		/*
		 * TODO: create a "start" option?
		 *
		 * For now, start test ~4 rtt (rounded to seconds + 1 sec)
		 * from now.
		 * 2 session requests, 1 startsessions command, 1 later.
		 */
		if(!OWPGetTimeOfDay(&test_spec.start_time)){
			I2ErrLogP(eh,errno,"Unable to get current time:%M");
			exit(1);
		}
		tvaladd(&delay_tval,&delay_tval);
		tvaladd(&delay_tval,&delay_tval);
		test_spec.start_time.sec += delay_tval.tv_sec+1;

		/*
		 * Prepare paths for datafiles. Unlink if not keeping data.
		 */
		
		if(ping_ctx.opt.to) {
			if (!OWPSessionRequest(ping_ctx.cntrl, NULL, False,
				       OWPAddrByNode(ctx,ping_ctx.remote_test),
				       True, (OWPTestSpec*)&test_spec, tosid, 
				       NULL, &err_ret))
			FailSession(ping_ctx.cntrl);
		}



		if(ping_ctx.opt.from) {

			if (ping_ctx.opt.save_from_test) {
				fromfp = fopen(ping_ctx.opt.save_from_test,
									"wb+");
				if(!fromfp){
					I2ErrLog(eh,"fopen(%s):%M", 
						ping_ctx.opt.save_from_test);
					exit(1);
				}
			} else {
				int	fd;
				char *path = strdup(OWP_TMPFILE);
				if(!path){
					I2ErrLog(eh,"strdup():%M");
					exit(1);
				}
				if((fd = mkstemp(path)) < 0){
					I2ErrLog(eh,"mkstemp(%s):%M",path);
					exit(1);
				}
				if(!(fromfp = fdopen(fd,"wb+"))){
					I2ErrLog(eh,"fdopen():%M");
					exit(1);
				}
				if(unlink(path) < 0){
					I2ErrLog(eh,"unlink(%s):%M",path);
				}
				free(path);
			}

			if (!OWPSessionRequest(ping_ctx.cntrl,
				       OWPAddrByNode(ctx,ping_ctx.remote_test),
				       True, NULL, False, 
				       (OWPTestSpec*)&test_spec, fromsid,
				       fromfp, &err_ret))
				FailSession(ping_ctx.cntrl);
		}
		
		local = OWPAddrByLocalControl(ping_ctx.cntrl); /* sender */

		if(OWPStartSessions(ping_ctx.cntrl) < OWPErrINFO)
			FailSession(ping_ctx.cntrl);

		/*
		 * TODO install sig handler for keyboard interupt - to send 
		 * stop sessions. (Currently SIGINT causes everything to be 
		 * killed and lost - might be reasonable to keep it that
		 * way...)
		 */
		if(OWPStopSessionsWait(ping_ctx.cntrl,NULL,&acceptval,&err))
			exit(1);

		if (acceptval != 0) {
			I2ErrLog(eh, "Test session(s) Questionable...");
		}

		if (ping_ctx.opt.to || ping_ctx.opt.from) {
			char *ptr;
			OWPAddr2string(local, local_str, sizeof(local_str));
			remote = strdup(ping_ctx.remote_test);
			if (!remote) {
			   I2ErrLog(eh, "Failed to copy remote host name: %M");
			   remote = "";
			} else {
				if ((ptr = strrchr(remote, ':')))
					*ptr = '\0';
			}
		}
		
		if(ping_ctx.opt.to)
			owp_fetch_sid(ping_ctx.opt.save_to_test,conndata.cntrl,
				      tosid, &state, local_str, remote, 1);

		if(ping_ctx.opt.from){
			if(do_records_all(fromfp,&state,remote,local_str)<0){
				I2ErrLog(eh,"do_records_all(from session):%M");
				exit(1);
			}
			if(fclose(fromfp) != 0){
				I2ErrLog(eh,"close():%M");
			}
		}
		
		exit(0);

	}

	if (!strcmp(progname, "owstats")) {
		FILE	*fp;
		u_int32_t hdr_len, num_rec;

		if(!(fp = fopen(argv[0],"rb"))){
			I2ErrLog(eh,"fopen(%s):%M",argv[0]);
			exit(1);
		}

		if (!(num_rec = OWPReadDataHeader(fp,&hdr_len))) {
			I2ErrLog(eh,"do_records_all() failed.");
			exit(1);
		}
		
		if (do_records_all(fp,&state,NULL,NULL) < 0){
			I2ErrLog(eh,"do_records_all() failed.");
			exit(1);
		}
		exit(0);
	}
	
	if (!strcmp(progname, "owfetch")) {
		int i;
		if (argc%2 == 0) {
			usage(progname, NULL);
			exit(1);
		}

		ping_ctx.remote_serv = argv[0];
		argv++;
		argc--;

		owp_set_auth(&ping_ctx, &policy, progname, ctx); 
		conndata.policy = policy;

		/*
		 * Open connection to owampd.
		 */
		ping_ctx.cntrl = OWPControlOpen(ctx, 
			OWPAddrByNode(ctx, ping_ctx.opt.srcaddr),
			OWPAddrByNode(ctx, ping_ctx.remote_serv),
			ping_ctx.auth_mode, 
			ping_ctx.opt.identity,
			(void*)&conndata, &err_ret);
		if (!ping_ctx.cntrl){
			I2ErrLog(eh, "Unable to open control connection.");
			exit(1);
		}
		conndata.cntrl = ping_ctx.cntrl;

		for (i = 0; i < argc/2; i++) {
			OWPSID sid;
			OWPHexDecode(argv[i], sid, 16);
			owp_fetch_sid(argv[i+1], conndata.cntrl, sid, &state,
				      NULL, NULL, 0);
		}
	}	

	exit(0);
}
