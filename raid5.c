#include <stdio.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

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

unsigned int device_count;
device *devices;
char buffer[SECTOR_SIZE] = {0};

//
// Function Declarations
//

void try_reopen_device(unsigned int device_number);
void close_device(unsigned int device_number);
void read_raid5(unsigned int logical_sector);
void write_raid5(unsigned int logical_sector);
void print_operation(unsigned int device_number, unsigned int physical_sector);
void print_bad_operation(unsigned int device_number);
bool read_sector(unsigned int device_number, unsigned int physical_sector);
bool write_sector(unsigned int device_number, unsigned int physical_sector);

//
// Implementation
//

int main(int argc, char** argv)
{
	if (argc == 1) {
		printf("Usage: ./raid5 <device1> <device2> <device3> [...]\n");
		return 0;
	}
	
	device_count = argc - 1;
	if (device_count <= 2) {
		printf("Error, raid5 not supported on less then 3 disks\n");
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
	while (fgets(line, sizeof(line), stdin) != NULL) {
		sscanf(line, "%s %d", cmd, &param);

		// KILL specified device
		if (!strcmp(cmd, "KILL")) {
			if ((param >= 0 && param < device_count)) {
				close_device(param);
			} else {
				printf("Error, invalid device number\n");
			}
		}
		// REPAIR
		else if (!strcmp(cmd, "REPAIR")) {
			if ((param >= 0 && param < device_count)) {
				try_reopen_device(param);
			} else {
				printf("Error, invalid device number\n");
			}
		}
		// READ
		else if (!strcmp(cmd, "READ")) {
			if (param >= 0) {
				read_raid5(param);
			} else {
				printf("Error, Invalid sector number\n");
			}
		}
		//WRITE
		else if (!strcmp(cmd, "WRITE")) {
			if (param >= 0) {
				write_raid5(param);
			} else {
				printf("Error, Invalid sector number\n");
			}
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
		printf("Error, failed to open device %s. (%s)\n", dev->path, strerror(errno));
		return;
	}
	dev->is_open = TRUE;
}

void close_device(unsigned int device_number)
{
	device* dev = &devices[device_number];
	if (dev->is_open) {
		close(dev->fd);
		dev->is_open = FALSE;
	}
}

void read_raid5(unsigned int logical_sector)
{
	// Number of the block on which the logical sector is placed.
	unsigned int block_num = logical_sector / SECTORS_PER_BLOCK;
	// Offset of the sector within the block
	unsigned int sector_block_offset = logical_sector % SECTORS_PER_BLOCK;
	// The stripe on which the block is placed
	unsigned int stripe_num = block_num / (device_count - 1);
	// Offset of physical sector
	unsigned int physical_sector = stripe_num * SECTORS_PER_BLOCK + sector_block_offset;
	// Device number of on which the parity block is located
	unsigned int parity_device =  ((device_count - 1) + stripe_num) % device_count;
	// Device number on which the requested logical sector is stored
	unsigned int sector_device = logical_sector % (device_count - 1);
	sector_device += (sector_device >= parity_device) ? 1 : 0;

	// Try reading from original sector
	if (devices[sector_device].is_open && read_sector(sector_device, physical_sector)) {
		return;
	}
	// Sector device is dead or read failed.

	// Try reading the other devices.
	// (it's possible to restore the logical sector using the parity block)
	for (unsigned int i = 0; i < device_count; ++i) {
		if (i == sector_device) continue;
		if (!devices[i].is_open  || !read_sector(i, physical_sector)) {
			// Logical sector can't be restored.
			print_bad_operation(i);
			return;
		}
	}
}

void write_raid5(unsigned int logical_sector)
{
	// Number of the block on which the logical sector is placed.
	unsigned int block_num = logical_sector / SECTORS_PER_BLOCK;
	// Offset of the sector within the block
	unsigned int sector_block_offset = logical_sector % SECTORS_PER_BLOCK;
	// The stripe on which the block is placed
	unsigned int stripe_num = block_num / (device_count - 1);
	// Offset of physical sector
	unsigned int physical_sector = stripe_num * SECTORS_PER_BLOCK + sector_block_offset;
	// Device number of on which the parity block is located
	unsigned int parity_device =  ((device_count - 1) + stripe_num) % device_count;
	// Device number on which the requested logical sector is stored
	unsigned int sector_device = logical_sector % (device_count - 1);
	sector_device += (sector_device >= parity_device) ? 1 : 0;

	// Try accessing sector device
	if (devices[sector_device].is_open) {
		// If parity device isn't open
		if (!devices[parity_device].is_open) {
			// Then there is no need to read the old data before writing,
			// so simply attempt writing to the sector device.
			if (!write_sector(sector_device, physical_sector)) {
				// Writing failed
				print_bad_operation(sector_device);
			}
			// Whether writing was successful or not, there's nothing else to be done.
			return;
		}
		// Parity device is open
		else {
			// Read parity
			if (!read_sector(parity_device, physical_sector)) {
				// Can't read parity, so there is no need to read the old data before writing
				// (because parity can't be updated),
				// so simply attempt writing to the sector device.
				if (!write_sector(sector_device, physical_sector)) {
					// Writing failed
					print_bad_operation(sector_device);
				}
				// Whether writing was successful or not, there's nothing else to be done.
				return;
			}

			// Read the old data, in order to update parity block later.
			if (read_sector(sector_device, physical_sector)) {
				//TODO: reorder writes??
				// Now write new data.
				// Note: return value isn't checked, because whether
				// the operation succeeds or not, the parity block still has to be updated.
				write_sector(sector_device, physical_sector);

				// new_parity = old_parity XOR old_data XOR new_data
				// Write new parity
				if (!write_sector(parity_device, physical_sector)) {
					// Accessing parity device failed
					print_bad_operation(parity_device);

				}
				// Whether writing was successful or not, there's nothing else to be done.
				return;
			}
		}
	}

	// Old data could not be read,
	// so we must read all other devices in order to update parity.
	for (unsigned int i = 0; i < device_count; ++i)
	{
		if (i == sector_device || i == parity_device) continue;
		if (!devices[i].is_open || !read_sector(i, physical_sector)) {
			// Can't calculate new parity.
			print_bad_operation(i);
			return;
		}
	}

	// new_parity = new_data XOR (XOR of all non-parity blocks)
	// Write new parity
	if (!devices[parity_device].is_open || !write_sector(parity_device, physical_sector)) {
		// Parity update failed.
		print_bad_operation(parity_device);
	}
}

void print_operation(unsigned int device_number, unsigned int physical_sector)
{
	printf("Operation on device %d, sector %d\n", device_number, physical_sector);
}

void print_bad_operation(unsigned int device_number)
{
	printf("Operation on bad device %d\n", device_number);
}

bool read_sector(unsigned int device_number, unsigned int physical_sector)
{
	device* dev = &devices[device_number];

	assert(dev->is_open);

	if (-1 == lseek(dev->fd, physical_sector * SECTOR_SIZE, SEEK_SET)) {
		printf("Error seeking to sector %d in device %s: %s\n", physical_sector, dev->path, strerror(errno));
		close_device(device_number);
		return FALSE;
	}

	ssize_t bytes_read = read(dev->fd, buffer, sizeof(buffer));
	if (bytes_read != sizeof(buffer)) {
		printf("Error reading from sector %d in device %s: %s\n", physical_sector, dev->path, strerror(errno));
		close_device(device_number);
		return FALSE;
	}

	print_operation(device_number, physical_sector);

	return TRUE;
}

bool write_sector(unsigned int device_number, unsigned int physical_sector)
{
	device* dev = &devices[device_number];

	assert(dev->is_open);

	if (-1 == lseek(dev->fd, physical_sector * SECTOR_SIZE, SEEK_SET)) {
		printf("Error seeking to sector %d in device %s: %s\n", physical_sector, dev->path, strerror(errno));
		close_device(device_number);
		return FALSE;
	}

	ssize_t bytes_written = write(dev->fd, buffer, sizeof(buffer));
	if (bytes_written != sizeof(buffer)) {
		printf("Error writing to sector %d in device %s: %s\n", physical_sector, dev->path, strerror(errno));
		close_device(device_number);
		return FALSE;
	}

	print_operation(device_number, physical_sector);

	return TRUE;
}

