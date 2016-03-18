// usage example:
// ./raid0 2

#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h> // for open flags
#include <unistd.h>
#include <time.h> // for time measurement
#include <sys/time.h>
#include <assert.h>
#include <errno.h> 
#include <string.h>

#define SECTORS_PER_BLOCK 4

int		num_dev;
int		*dev_status;

void do_raid0(int sector_log)
{
	// find the relevant device for current sector
	int block_num = sector_log / SECTORS_PER_BLOCK;
	int dev_num = block_num % num_dev;

	// make sure device didn't fail
	if (dev_status[dev_num] < 0) {
		printf("Operation on bad device %d\n", dev_num);
		return;
	}

	// find sector inside device
	int block_start = sector_log / (num_dev * SECTORS_PER_BLOCK);
	int block_off = sector_log % SECTORS_PER_BLOCK;
	int sector_phys = block_start * SECTORS_PER_BLOCK + block_off;

	printf("Operation on device %d, sector %d\n", dev_num, sector_phys);
}

int main(int argc, char** argv)
{
	assert(argc == 2);
	
	int i;
	char line[1024];
	
	// number of devices == number of arguments (ignore 1st)
	sscanf(argv[1], "%d", &num_dev);
	int _dev_status[num_dev];
	dev_status = _dev_status;
	
	// open all devices
	for (i = 0; i < num_dev; ++i)
		dev_status[i] = 1;
	
	// vars for parsing input line
	char cmd[20];
	int param;
	
	// read input lines to get command of type "<CMD> <PARAM>"
	while (fgets(line, 1024, stdin) != NULL) {
		sscanf(line, "%s %d", cmd, &param);

		// KILL specified device
		if (!strcmp(cmd, "KILL")) {
			dev_status[param] = -1;
		}
		// OP
		else if (!strcmp(cmd, "OP")) {
			do_raid0(param);
		}
		else {
			printf("Invalid command: %s\n", cmd);
		}
	}
}
