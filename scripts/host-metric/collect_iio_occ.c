#include <stdio.h>				// printf, etc
#include <stdint.h>				// standard integer types, e.g., uint32_t
#include <signal.h>				// for signal handler
#include <stdlib.h>				// exit() and EXIT_FAILURE
#include <string.h>				// strerror() function converts errno to a text string for printing
#include <fcntl.h>				// for open()
#include <errno.h>				// errno support
#include <assert.h>				// assert() function
#include <unistd.h>				// sysconf() function, sleep() function
#include <sys/mman.h>			// support for mmap() function
#include <math.h>				// for pow() function used in RAPL computations
#include <time.h>
#include <sys/time.h>			// for gettimeofday
#include <sys/ipc.h>
#include <sys/shm.h>

#define LOG_FREQUENCY 1
#define LOG_PRINT_FREQUENCY 20
#define LOG_SIZE 100000
#define WEIGHT_FACTOR 8
#define WEIGHT_FACTOR_LONG_TERM 256
#define IRP_MSR_PMON_CTL_BASE 0x0A5BL
#define IRP_MSR_PMON_CTR_BASE 0x0A59L
#define IIO_PCIE_1_PORT_0_BW_IN 0x0B20 //We're concerned with PCIe 1 stack on our machine (Table 1-11 in Intel Skylake Manual)
#define STACK 1 //We're concerned with stack #2 on our machine
#define IRP_OCC_VAL 0x0040040F
// #define CORE 20
#define NUM_LPROCS 80

int coreid;
int msr_fd[NUM_LPROCS];		// msr device driver files will be read from various functions, so make descriptors global

FILE *log_file;

struct log_entry{
	uint64_t l_tsc; //latest TSC
	uint64_t td_ns; //latest measured time delta in us
	double avg_occ; //latest measured avg IIO occupancy
	double s_avg_occ; //latest calculated smoothed occupancy
	double s_avg_occ_longterm; //latest calculated smoothed occupancy long term
	int cpu; //current cpu
};

struct log_entry LOG[LOG_SIZE];
uint32_t log_index = 0;
uint32_t counter = 0;
uint64_t prev_rdtsc = 0;
uint64_t cur_rdtsc = 0;
uint64_t start_cur_rdtsc = 0;
uint64_t prev_cum_occ = 0;
uint64_t cur_cum_occ = 0;
uint64_t prev_cum_frc = 0;
uint64_t cur_cum_frc = 0;
uint64_t tsc_sample = 0;
uint64_t msr_num;
uint64_t msr_val;
uint64_t rc64;
uint64_t start_cum_occ_sample = 0;
uint64_t cum_occ_sample = 0;
uint64_t cum_frc_sample = 0;
double latest_avg_occ = 0;
uint64_t latest_avg_pcie_bw = 0;
uint64_t latest_time_delta = 0;
double smoothed_avg_occ = 0;
double smoothed_avg_occ_longterm = 0;
double smoothed_avg_pcie_bw = 0;
double smoothed_avg_occ_f = 0.0;
double smoothed_avg_occ_longterm_f = 0.0;
double smoothed_avg_pcie_bw_f = 0.0;
uint64_t latest_time_delta_us = 0;
uint64_t latest_time_delta_ns = 0;

static inline __attribute__((always_inline)) unsigned long rdtsc()
{
   unsigned long a, d;

   __asm__ volatile("rdtsc" : "=a" (a), "=d" (d));

   return (a | (d << 32));
}


static inline __attribute__((always_inline)) unsigned long rdtscp()
{
   unsigned long a, d, c;

   __asm__ volatile("rdtscp" : "=a" (a), "=d" (d), "=c" (c));

   return (a | (d << 32));
}

extern inline __attribute__((always_inline)) int get_core_number()
{
   unsigned long a, d, c;

   __asm__ volatile("rdtscp" : "=a" (a), "=d" (d), "=c" (c));

   return ( c & 0xFFFUL );
}

void rdmsr_userspace(int core, uint64_t rd_msr, uint64_t *rd_val_addr){
    rc64 = pread(msr_fd[core],rd_val_addr,sizeof(rd_val_addr),rd_msr); //write mem[rd_val_addr] to offset rd_msr in msr_fd[core]
    if (rc64 != sizeof(rd_val_addr)) {
        fprintf(log_file,"ERROR: failed to read MSR %lx on Logical Processor %d", rd_msr, core);
        exit(-1);
    }
}

void wrmsr_userspace(int core, uint64_t wr_msr, uint64_t *wr_val_addr){
    rc64 = pwrite(msr_fd[core],wr_val_addr,sizeof(wr_val_addr),wr_msr);//write mem[wr_val_addr] to offset wr_msr in msr_fd[core]
    if (rc64 != 8) {
        fprintf(log_file,"ERROR writing to MSR device on core %d, write %ld bytes\n",core,rc64);
        exit(-1);
    }
}

static void update_log(int c){
	LOG[log_index % LOG_SIZE].l_tsc = cur_rdtsc;
	LOG[log_index % LOG_SIZE].td_ns = latest_time_delta_ns;
	LOG[log_index % LOG_SIZE].avg_occ = latest_avg_occ;
	LOG[log_index % LOG_SIZE].s_avg_occ = smoothed_avg_occ;
	LOG[log_index % LOG_SIZE].s_avg_occ_longterm = smoothed_avg_occ_longterm;
	LOG[log_index % LOG_SIZE].cpu = c;
	log_index++;
}

/*write CTL register of CORE*/
static void update_occ_ctl_reg(void){
	//program the desired CTL register to read the corresponding CTR value
	msr_num = IRP_MSR_PMON_CTL_BASE + (0x20 * STACK) + 0;
    uint64_t wr_val = IRP_OCC_VAL;
	wrmsr_userspace(coreid,msr_num,&wr_val);
}

/*read CTR register of core c to update cur_cum_occ and prev_cum_occ*/
static void sample_iio_occ_counter(int c){
    uint64_t rd_val = 0;
	msr_num = IRP_MSR_PMON_CTR_BASE + (0x20 * STACK) + 0;
	rdmsr_userspace(c,msr_num,&rd_val);
	cum_occ_sample = rd_val;
	prev_cum_occ = cur_cum_occ;
	cur_cum_occ = cum_occ_sample;
}

/*update cur_rdtsc and prev_rdtsc*/
static void sample_time_counter(){
    tsc_sample = rdtscp();
	prev_rdtsc = cur_rdtsc;
	cur_rdtsc = tsc_sample;
}

/*update occ and rdtsc of core c*/
static void sample_counters(int c){
	//first sample occupancy
	sample_iio_occ_counter(c);
	//sample time at the last
	sample_time_counter();
	return;
}

static void update_occ(void){
	// latest_time_delta_us = (cur_rdtsc - prev_rdtsc) / 3300;
	latest_time_delta_ns = ((cur_rdtsc - prev_rdtsc) * 10) / 21;
	if(latest_time_delta_ns > 0){
		latest_avg_occ = (cur_cum_occ - prev_cum_occ) / (double) latest_time_delta_ns;
        // if(latest_avg_occ > 10){
            // fprintf(log_file,"%d:in\n", log_index);
            smoothed_avg_occ_f = ((((double) (WEIGHT_FACTOR-1))*smoothed_avg_occ_longterm_f) + latest_avg_occ) / ((double) WEIGHT_FACTOR);
            smoothed_avg_occ = smoothed_avg_occ_f;

            smoothed_avg_occ_longterm_f = ((((double) (WEIGHT_FACTOR_LONG_TERM-1))*smoothed_avg_occ_longterm_f) + latest_avg_occ) / ((double) WEIGHT_FACTOR_LONG_TERM);
            smoothed_avg_occ_longterm = smoothed_avg_occ_longterm_f;
            // fprintf(log_file,"%d:avg_occ = %ld, occ = %ld, occ_longterm = %ld\n", log_index, latest_avg_occ, smoothed_avg_occ, smoothed_avg_occ_longterm);
        // }
		// smoothed_avg_occ = ((7*smoothed_avg_occ) + latest_avg_occ) >> 3;
	}
	// (float(occ[i] - occ[i-1]) / ((float(time_us[i+1] - time_us[i])) * 1e-6 * freq)); 
}

void main_init() {
    //initialize the log
    int i=0;
    while(i<LOG_SIZE){
        LOG[i].l_tsc = 0;
        LOG[i].td_ns = 0;
        LOG[i].avg_occ = 0;
        LOG[i].s_avg_occ = 0;
        LOG[i].s_avg_occ_longterm = 0;
        LOG[i].cpu = 81;
        i++;
    }
    update_occ_ctl_reg();
}

/*print log info in log_file*/
void main_exit() {
    //dump log info
    int i=0;
    sample_counters(coreid);
    uint64_t time_delta_ns = ((cur_rdtsc - start_cur_rdtsc) * 10) / 21;
    uint64_t cum_occ = (cum_occ_sample - start_cum_occ_sample);
    double avg_occ = (double) cum_occ / time_delta_ns;
    fprintf(log_file, "average occ = %.5lf\n", avg_occ);
    fprintf(log_file, "total counter = %d\n", counter);
    fprintf(log_file,
    "index,latest_tsc,time_delta_ns,avg_occ,s_avg_occ,s_avg_occ_long,cpu\n");
    while(i<LOG_SIZE){
        fprintf(log_file,"%d,%ld,%ld,%lf,%lf,%lf,%d\n",
        i,
        LOG[i].l_tsc,
        LOG[i].td_ns,
        LOG[i].avg_occ,
        LOG[i].s_avg_occ,
        LOG[i].s_avg_occ_longterm,
        LOG[i].cpu);
        i++;
    }
}

static void catch_function(int signal) {
	printf("Caught SIGCONT. Shutting down...\n");
    main_exit();
	exit(0);
}

int main(int argc, char* argv[]){
    if (signal(SIGINT, catch_function) == SIG_ERR) {
	    printf("An error occurred while setting the signal handler.\n");
		return EXIT_FAILURE;
	}

    char filename[100];
    sprintf(filename,"iio.log");
    log_file = fopen(filename,"w+");
    if (log_file == 0) {
        fprintf(stderr,"ERROR %s when trying to open log file %s\n",strerror(errno),filename);
        exit(-1);
    }

    int nr_cpus = NUM_LPROCS;
    int i;
    for (i=0; i<nr_cpus; i++) {
		sprintf(filename,"/dev/cpu/%d/msr",i);
		msr_fd[i] = open(filename, O_RDWR);
		// printf("   open command returns %d\n",msr_fd[i]);
		if (msr_fd[i] == -1) {
			fprintf(log_file,"ERROR %s when trying to open %s\n",strerror(errno),filename);
			exit(-1);
		}
	}

    // int cpu = get_core_number();
    coreid = atoi(argv[1]);
    main_init();
    sample_counters(coreid);
    start_cum_occ_sample = cum_occ_sample;
    start_cur_rdtsc = cur_rdtsc;
    
    while(1){
        sample_counters(coreid);
        update_occ();
        update_log(coreid);
        counter++;
    }

    main_exit();
    return 0;
}