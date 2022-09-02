#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>
#include <sys/syscall.h>
#include "testcases.h"

#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>

void *buff;
unsigned long nr_signals = 0;

#define PAGE_SIZE		(4096)
#define SIGBALLOON		40
#define ID 				442
#define SWAP			443
#define M 				256
#define MAX_SWAP		(32 * M)
#define Min(a, b) (a > b? b: a)


int fd1, fd2;
uint64_t list[MAX_SWAP];
int size;

uint64_t range;
uint64_t width;
int isSetIdle;
uint64_t current, current2;
uint64_t begin_addr;

uint64_t nextCurrent(uint64_t* cur){
	*cur += PAGE_SIZE;
	if(*cur == (begin_addr + TOTAL_MEMORY_SIZE))
		*cur = begin_addr;
}

void openfiles(){
	// reading process pagemap
	char file1[100];
	snprintf(file1, sizeof(file1), "/proc/%d/pagemap", getpid());
	fd1 = open(file1, O_RDONLY);
	if(fd1 < 0) {
	    perror("unable to open pagemap");
	    return;
	}

	// reading process pagemap
	char file2[100] = "/sys/kernel/mm/page_idle/bitmap";
	fd2 = open(file2, O_RDWR);
	if(fd2 < 0) {
	    perror("unable to open bitmap");
	    return;
	}	
}

void closefiles(){
	close(fd1);
	close(fd2);
	fd1 = fd2 = -1;
}

// just defined for testing
void printStats(){
	uint64_t total = 0, idle = 0, present = 0, swapped = 0;
	int len = sizeof(uint64_t);
	int mycnt = 0;

	uint64_t data, pfn;
	uint64_t begin_addr = buff;
	uint64_t last_addr = begin_addr + TOTAL_MEMORY_SIZE;
	uint64_t index = (begin_addr / PAGE_SIZE) * len;


	for(uint64_t addr = begin_addr; addr < last_addr; addr += PAGE_SIZE, index += len){
		
		if(pread(fd1, &data, len, index) != len) {
			perror("error in pread in pagemap");
		    return;
		}

		total ++;
		if((data >> 63) & 1) present++;
		if((data >> 62) & 1) swapped++;

		// if page is not present
		if(((data >> 63) & 1) != 1) continue;

		// obtain the page frame number
		pfn = data & 0x7fffffffffffff;

		// check whether the pfn is idle or not in the bitmap
		int idx = pfn / 64;
		int offset = pfn % 64;

		// read the page_idle bitmap
		if(pread(fd2, &data, len, idx * len) != len) {
			perror("error in pread in bitmap");
		    return;
		}

		// if the frame is idle 
		// increment the idle
		uint64_t x = 1;
		uint64_t mask = x << offset;
		if(data & mask) idle++;
	}

	printf("total = %ld, present = %ld\nswapped = %ld, idle = %ld\n", total, present, swapped, idle);
}

void setIdle(){
	int len = sizeof(uint64_t);
	uint64_t data, pfn;
	uint64_t index;
	uint64_t i;

	size = 0;
	isSetIdle = 0;

	for(i = 0; i < width; i++){
		index = (current / PAGE_SIZE) * len;

		if(pread(fd1, &data, len, index) != len) {
			perror("error in pread in pagemap");
		    return;
		}

		// if page is not present
		if(((data >> 63) & 1) != 1) {
			nextCurrent(&current);
			continue;
		}

		// obtain the page frame number
		pfn = data & 0x7fffffffffffff;

		// check whether the pfn is idle or not in the bitmap
		int idx = pfn / 64;
		int offset = pfn % 64;

		data = 1;
		data <<= offset;

		// mark as idle
		if(pwrite(fd2, &data, len, idx * len) != len){
			perror("error in write in bitmap");
    		return;			
		}
		if(size < MAX_SWAP){
			list[size] = current;
			size++;
		}

		nextCurrent(&current);
	}
}

void suggestIdleFrames(){
	size = 0;
	int len = sizeof(uint64_t);
	uint64_t data, pfn;
	uint64_t index;


	while(current2 != current){
		if(size == MAX_SWAP) return;

		index = (current2 / PAGE_SIZE) * len;

		if(pread(fd1, &data, len, index) != len) {
			perror("error in pread in pagemap");
		    return;
		}

		// if page is not present
		if(((data >> 63) & 1) != 1) {
			nextCurrent(&current2);
			continue;
		}

		// obtain the page frame number
		pfn = data & 0x7fffffffffffff;

		// check whether the pfn is idle or not in the bitmap
		int idx = pfn / 64;
		int offset = pfn % 64;

		// // read the page_idle bitmap
		if(pread(fd2, &data, len, idx * len) != len) {
			perror("error in pread in bitmap");
		    return;
		}

		uint64_t mask = 1;
		mask <<= offset;

		// if idle then suggest this curr_addr
		if(mask & data){
			list[size] = current2;
			size++;
		}

		nextCurrent(&current2);
	}
	isSetIdle = 1;
}


void signalHandler(int sig){
	nr_signals++; 
	if(fd1 < 0) return;

	if(isSetIdle) setIdle();
	else suggestIdleFrames();
	syscall(SWAP, list, size);
}



int main(int argc, char *argv[])
{
	int *ptr, nr_pages;

    	ptr = mmap(NULL, TOTAL_MEMORY_SIZE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

	if (ptr == MAP_FAILED) {
		printf("mmap failed\n");
       		exit(1);
	}
	buff = ptr;
	memset(buff, 0, TOTAL_MEMORY_SIZE);	
    
	
	// user space

	// Intitalizer
	openfiles();
	current =  current2 = begin_addr = buff;
	range = Min((1024 * 1024 * 1024), TOTAL_MEMORY_SIZE);
	width = range / PAGE_SIZE;
	isSetIdle = 1;


	// register with the SIGBALLOON signal
    signal(SIGBALLOON, signalHandler);

	// register the process with ballooning subsystem
	clock_t t;
    t = clock();
	syscall(ID);	
	t = clock() - t;
    printf("System call returned in %f seconds.\n",((float)t) / CLOCKS_PER_SEC);
 

	/* test-case */
	test_case_main(buff, TOTAL_MEMORY_SIZE);

	// closing open files
	closefiles();

	munmap(ptr, TOTAL_MEMORY_SIZE);

	printf("I received SIGBALLOON %lu times\n", nr_signals);
	
}










