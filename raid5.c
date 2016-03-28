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
void calc_offsets_raid5(unsigned int logical_sector,
		unsigned int* data_device,
		unsigned int* parity_device,
		unsigned int* physical_sector);
void slow_write(unsigned int data_device,
		unsigned int parity_device,
		unsigned int physical_sector);
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
	device* dev = &devices[device_number];

	// Open device
	int fd = open(dev->path, O_RDWR);
	if (fd == -1) {
		printf("Error, failed to open device %s. (%s)\n", dev->path, strerror(errno));
		return;
	}

	// Close previous device,
	// And update data structure.
	close_device(device_number);
	dev->fd = fd;
	dev->is_open = TRUE;
}

void close_device(unsigned int device_number)
//
// Close the device if it's open. otherwise do nothing.
//
{
	device* dev = &devices[device_number];
	if (dev->is_open) {
		close(dev->fd);
		dev->is_open = FALSE;
	}
}

void read_raid5(unsigned int logical_sector)
{
	unsigned int data_device;
	unsigned int parity_device;
	unsigned int physical_sector;
	calc_offsets_raid5(logical_sector, &data_device, &parity_device, &physical_sector);


	// Try reading from original sector
	if (read_sector(data_device, physical_sector)) {
		return;
	}
	// Sector device is dead or read failed.

	// Try reading the other devices.
	// (it's possible to restore the logical sector using the parity block)
	for (unsigned int i = 0; i < device_count; ++i) {
		if (i == data_device) continue;
		if (!read_sector(i, physical_sector)) {
			// Logical sector can't be restored.
			print_bad_operation(i);
			return;
		}
	}
}

void write_raid5(unsigned int logical_sector)
{
	unsigned int data_device;
	unsigned int parity_device;
	unsigned int physical_sector;
	calc_offsets_raid5(logical_sector, &data_device, &parity_device, &physical_sector);

	// If parity device is closed
	if (!devices[parity_device].is_open) {
		// Try writing to the data device directly, and finish.
		// (there is nothing else to do if it fails)
		if (!write_sector(data_device, physical_sector)) {
			print_bad_operation(data_device);
		}
		return ;
	}
	assert(devices[parity_device].is_open);

	// If data device is closed
	if (!devices[data_device].is_open) {
		// parity can only be updated by a "slow write"
		slow_write(data_device, parity_device, physical_sector);
		return;
	}

	//NOTE: I know this code looks weird,
	// but it had to be this way in order to follow the instructions
	// that stated that operations should be carried out in order of device numbers.
	if (data_device < parity_device) {
		if (!read_sector(data_device, physical_sector)) {
			slow_write(data_device, parity_device, physical_sector);
			return;
		}
		if (!write_sector(data_device, physical_sector)) {
			// Since the old data was read, the parity can still be updated efficiently.
			if (!read_sector(parity_device, physical_sector)
					|| !write_sector(parity_device, physical_sector)) {
				print_bad_operation(parity_device);
				return;
			}
		}
		// Data was written successfully.
		// Try to update parity. (but don't report failure).
		read_sector(parity_device, physical_sector);
		write_sector(parity_device, physical_sector);
		return;
	}

	assert(parity_device < data_device);

	if (!read_sector(parity_device, physical_sector)
			|| !write_sector(parity_device, physical_sector)) {
		// Parity can't be updated, so just write the new data
		// (no need to read the old data)
		if (!write_sector(data_device, physical_sector)) {
			print_bad_operation(data_device);
		}
		return;
	}

	if (read_sector(data_device, physical_sector)) {
		// Since the old data was read, then the parity could actually be updated,
		// and the data is logically written.
		// So just try to write the data to it's intended sector,
		// but it doesn't matter if the write is successful or not.
		write_sector(data_device, physical_sector);
		return;
	} else {
		// Old data can't be read, so the parity block wasn't actually updated.
		// Try updating it by a "slow write".
		slow_write(data_device, parity_device, physical_sector);
		return;
	}

}

void slow_write(unsigned int data_device,
		unsigned int parity_device,
		unsigned int physical_sector)
// Write new data to parity block, without having the old data.
{
	for (unsigned int i = 0; i < device_count; ++i)
	{
		// Skip data device
		if (i == data_device) continue;

		if (i == parity_device) {
			if (!write_sector(i, physical_sector)) {
				print_bad_operation(i);
				return;
			}
		} else {
			if (!read_sector(i, physical_sector)) {
				print_bad_operation(i);
				return;
			}
		}
	}
}

//NOTE: I implemented write_raid5 before clarifications about operation order were given by the TA.
// This is the old implementation, and i left it here just for reference.
void write_raid5_(unsigned int logical_sector)
{
	unsigned int data_device;
	unsigned int parity_device;
	unsigned int physical_sector;
	calc_offsets_raid5(logical_sector, &data_device, &parity_device, &physical_sector);

	// Try accessing sector device
	if (devices[data_device].is_open) {
		// If parity device isn't open
		if (!devices[parity_device].is_open) {
			// Then there is no need to read the old data before writing,
			// so simply attempt writing to the sector device.
			if (!write_sector(data_device, physical_sector)) {
				// Writing failed
				print_bad_operation(data_device);
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
				if (!write_sector(data_device, physical_sector)) {
					// Writing failed
					print_bad_operation(data_device);
				}
				// Whether writing was successful or not, there's nothing else to be done.
				return;
			}

			// Read the old data, in order to update parity block later.
			if (read_sector(data_device, physical_sector)) {
				// Now write new data.
				// Note: return value isn't checked, because whether
				// the operation succeeds or not, the parity block still has to be updated.
				write_sector(data_device, physical_sector);

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
		if (i == data_device || i == parity_device) continue;
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

void calc_offsets_raid5(unsigned int logical_sector,
		unsigned int* data_device,
		unsigned int* parity_device,
		unsigned int* physical_sector)
//
// Given a logical sector,
// calculate the device number and physical sector offset that store it,
// and the corresponding parity-block device.
//
{
	// Number of the block on which the data of the logical sector is placed.
	unsigned int data_block_num = logical_sector / SECTORS_PER_BLOCK;
	// Offset of the sector within the block
	unsigned int data_sector_block_offset = logical_sector % SECTORS_PER_BLOCK;
	// The stripe on which the block is placed
	unsigned int stripe_num = data_block_num / (device_count - 1);

	// Offset of physical sector
	*physical_sector = stripe_num * SECTORS_PER_BLOCK + data_sector_block_offset;
	// Device number of on which the parity block is located
	*parity_device =  ((device_count - 1) - stripe_num) % device_count;
	// Device number on which the requested data block is stored
	*data_device = data_block_num % (device_count - 1);
	*data_device += (*data_device >= *parity_device) ? 1 : 0;

	assert(0 <= *data_device && *data_device < device_count);
	assert(0 <= *parity_device && *parity_device < device_count);
	assert(*parity_device != *data_device);
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
//
// Try reading a physical sector from a given device.
// If the device is closed, FALSE is returned.
// If an error happens, a message is printed, the device is closed, and FALSE is returned.
// Otherwise an operation message is printed, and TRUE is returned.
//
{
	device* dev = &devices[device_number];

	if (!dev->is_open) {
		return FALSE;
	}

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
//
// Try writing a physical sector to a given device.
// If the device is closed, FALSE is returned.
// If an error happens, a message is printed, the device is closed, and FALSE is returned.
// Otherwise an operation message is printed, and TRUE is returned.
//
{
	device* dev = &devices[device_number];

	if (!dev->is_open) {
		return FALSE;
	}

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

