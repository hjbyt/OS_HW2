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

//
// Macros
//

// Return number of elements in static array
#define ARRAY_LENGTH(array) (sizeof(array)/sizeof(array[0]))
//TODO: XXX
//// Exit with an error message
//#define ERROR(...) error(EXIT_FAILURE, errno, __VA_ARGS__)
//// Verify that a condition holds, else exit with an error.
//#define VERIFY(condition, ...) if (!(condition)) ERROR(__VA_ARGS__)

//
// Constants
//

#define KILO 1024
#define SECTORS_PER_BLOCK 4
#define SECTOR_SIZE (4 * KILO)
#define BLOCK_SIZE (SECTOR_SIZE * SECTORS_PER_BLOCK)

typedef int bool;
#define FALSE 0
#define TRUE 1

//
// Structs
//

typedef struct device_{
	const char* path;
	int fd;
	bool is_open;
} device;

//
// Globals
//

int device_count;
device *devices;
char buffer[SECTOR_SIZE] = {0};
//
// Function Declarations
//

void try_reopen_device(unsigned int device_number);
void close_device(int device_number);
bool read_sector(int device_number, int physical_sector);
bool write_sector(int device_number, int physical_sector);
void do_raid5(int sector_log);

//
// Implementation
//

int main(int argc, char** argv)
{
	if (argc == 1) {
		printf("Usage: ./raid5 <device1> <device2> <device3> [...]");
		return 0;
	}
	
	device_count = argc - 1;
	if (device_count <= 2) {
		printf("Error, raid5 not supported on less then 3 disks");
		return 0;
	}
	device _devices[device_count];
	devices = _devices;
	
	// try open all devices
	for (int i = 0; i < device_count; ++i) {
		devices[i].path = argv[i + 1];
		devices[i].is_open = FALSE;
		try_reopen_device(i);
	}
	
	// read input lines to get command of type "<CMD> <PARAM>"
	char line[1024];
	char cmd[20];
	int param;
	while (fgets(line, 1024, stdin) != NULL) {
		sscanf(line, "%s %d", cmd, &param);

		// KILL specified device
		if (!strcmp(cmd, "KILL")) {
			if ((param >= 0 && param < device_count)) {
				close_device(param);
			} else {
				printf("Error, Wrong device number");
			}
		}
		// REPAIR
		else if (!strcmp(cmd, "REPAIR")) {
			if ((param >= 0 && param < device_count)) {
				try_reopen_device(param);
			} else {
				printf("Error, Wrong device number");
			}
		}
		// READ
		else if (!strcmp(cmd, "READ")) {
			do_raid5(param);
		}
		//WRITE
		else if (!strcmp(cmd, "WRITE")) {
			do_raid5(param);
		}
		else {
			printf("Invalid command: %s\n", cmd);
		}
	}

	// close devices
	for (int i = 0; i < device_count; ++i) {
		close_device(i);
	}

	return 0;
}

void try_reopen_device(unsigned int device_number)
{
	assert(device_number < device_count);

	close_device(device_number);
	device* dev = &devices[device_number];
	dev->fd = open(dev->path, O_RDWR);
	if (dev->fd == -1) {
		printf("Error, failed to open device %s. (%s)", dev->path, strerror(errno));
		return;
	}
	dev->is_open = TRUE;
}

void close_device(int device_number)
{
	device* dev = &devices[device_number];
	if (dev->is_open) {
		close(dev->fd);
		dev->is_open = FALSE;
	}
}

void read_raid5(int logical_sector)
{
	int block_num = logical_sector / SECTORS_PER_BLOCK;
	// Offset of the sector within the block
	int sector_block_offset = logical_sector % SECTORS_PER_BLOCK;
	int stripe_num = block_num / (device_count - 1);
	// Offset of physical sector
	int physical_sector = stripe_num * SECTORS_PER_BLOCK + sector_block_offset;

	// Device number of on which the parity block is located
	int parity_device =  ((device_count - 1) + stripe_num) % device_count;
	// Device number on which the requested logical sector is stored
	int sector_device = logical_sector % (device_count - 1);
	sector_device += (sector_device >= parity_device) ? 1 : 0;

	// Try reading from original sector
	if (read_sector(sector_device, physical_sector)) {
		printf("Operation on device %d, sector %d\n", sector_device, physical_sector);
	} else {
		// Try to read from the other disks
		for (int i = 0; i < device_count; ++i) {
			if (i == sector_device) continue;
			if (read_sector(i, physical_sector)) {
				printf("Operation on device %d, sector %d\n", i, physical_sector);
			} else {
				//TODO: what to print ??
				printf("Operation on bad device %d\n", i);
				break;
			}
		}
	}
}

void write_raid5(int logical_sector)
{
	//TODO (FML)
}

bool read_sector(int device_number, int physical_sector)
{
	device* dev = &devices[device_number];

	if (-1 == lseek(dev->fd, physical_sector * SECTOR_SIZE, SEEK_SET)) {
		printf("Error seeking to sector %d in device %s: %s", physical_sector, dev->path, strerror(errno));
		close(device_number);
		return FALSE;
	}

	ssize_t bytes_read = read(dev->fd, buffer, sizeof(buffer));
	if (bytes_read != sizeof(buffer)) {
		printf("Error reading from sector %d in device %s: %s", physical_sector, dev->path, strerror(errno));
		close(device_number);
		return FALSE;
	}

	return TRUE;
}

bool write_sector(int device_number, int physical_sector)
{
	device* dev = &devices[device_number];

	if (-1 == lseek(dev->fd, physical_sector * SECTOR_SIZE, SEEK_SET)) {
		printf("Error seeking to sector %d in device %s: %s", physical_sector, dev->path, strerror(errno));
		close(device_number);
		return FALSE;
	}

	ssize_t bytes_written = write(dev->fd, buffer, sizeof(buffer));
	if (bytes_written != sizeof(buffer)) {
		printf("Error writing to sector %d in device %s: %s", physical_sector, dev->path, strerror(errno));
		close(device_number);
		return FALSE;
	}

	return TRUE;
}

void do_raid5(int sector_log)
{
	// find the relevant device for current sector
	int block_num = sector_log / SECTORS_PER_BLOCK;
	int dev_num = block_num % device_count;

	// make sure device didn't fail
	if (devices[device_count].is_open) {
		printf("Operation on bad device %d\n", dev_num);
		return;
	}

	// find sector inside device
	int block_start = sector_log / (device_count * SECTORS_PER_BLOCK);
	int block_off = sector_log % SECTORS_PER_BLOCK;
	int sector_phys = block_start * SECTORS_PER_BLOCK + block_off;

	printf("Operation on device %d, sector %d\n", dev_num, sector_phys);
}
