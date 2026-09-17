// i2c.c
//
// This code was originally derived from Marek Wyborski's i2cbb.c 
// and used to bit-bang I2C over GPIO.  We now use the kernel's own
// i2c subsystem and the bit-banging code is gone.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include "i2c.h"

static int i2c_fd = -1;

void i2c_init(int i2c_bus_number) {
	char path[32];
	snprintf(path, sizeof(path), "/dev/i2c-%d", i2c_bus_number);

	i2c_fd = open(path, O_RDWR);
	if (i2c_fd < 0) {
		printf("i2c: failed to open %s: %s - I2C calls will fail\n",
		       path, strerror(errno));
	}
}

// Minimal SMBus ioctl wrapper - see <linux/i2c-dev.h> for the protocol.
// addr is set per-call via I2C_SLAVE since each public function here
// already takes its own i2c_address argument (multiple devices share
// one open fd/bus, same as the old bit-banged API allowed). */
static int i2c_smbus_xfer(uint8_t addr, uint8_t read_write, uint8_t command,
                           int size, union i2c_smbus_data *data) {
	if (i2c_fd < 0)
		return -1;

	if (ioctl(i2c_fd, I2C_SLAVE, addr) < 0)
		return -1;

	struct i2c_smbus_ioctl_data args;
	args.read_write = read_write;
	args.command = command;
	args.size = size;
	args.data = data;
	return ioctl(i2c_fd, I2C_SMBUS, &args);
}

// This executes the SMBus "write byte" protocol, returning negative errno else zero on success.
int32_t i2c_write_byte_data(uint8_t i2c_address, uint8_t command, uint8_t value) {
	union i2c_smbus_data data;
	data.byte = value;

	if (i2c_smbus_xfer(i2c_address, I2C_SMBUS_WRITE, command,
	                    I2C_SMBUS_BYTE_DATA, &data) < 0) {
		printf("i2c: write byte failed (addr 0x%02x, reg 0x%02x): %s\n",
		       i2c_address, command, strerror(errno));
		return -1;
	}
	return 0;
}

// This executes the SMBus "read byte" protocol, returning negative errno else a data byte received from the device.
int32_t i2c_read_byte_data(uint8_t i2c_address, uint8_t command) {
	union i2c_smbus_data data;

	if (i2c_smbus_xfer(i2c_address, I2C_SMBUS_READ, command,
	                    I2C_SMBUS_BYTE_DATA, &data) < 0) {
		printf("i2c: read byte failed (addr 0x%02x, reg 0x%02x): %s\n",
		       i2c_address, command, strerror(errno));
		return -1;
	}
	return data.byte & 0xFF;
}

// This executes the SMBus "block write" protocol, returning negative errno else zero on success.
int32_t i2c_write_i2c_block_data(uint8_t i2c_address, uint8_t command, uint8_t length,
        const uint8_t * values) {
	if (length == 0) {
		// "Point the register pointer, no data" - a plain single-byte
		// write of just the command/register byte, same pattern
		// radio_hw.c uses to kick off a subsequent INA260 read.
		union i2c_smbus_data data; // unused for I2C_SMBUS_BYTE
		if (i2c_smbus_xfer(i2c_address, I2C_SMBUS_WRITE, command,
		                    I2C_SMBUS_BYTE, &data) < 0) {
			printf("i2c: address/command failed (addr 0x%02x, reg 0x%02x): %s\n",
			       i2c_address, command, strerror(errno));
			return -1;
		}
		return 0;
	}

	if (length > I2C_SMBUS_BLOCK_MAX) {
		printf("i2c: block write length %d exceeds max %d\n",
		       length, I2C_SMBUS_BLOCK_MAX);
		return -1;
	}

	union i2c_smbus_data data;
	data.block[0] = length;
	memcpy(&data.block[1], values, length);

	if (i2c_smbus_xfer(i2c_address, I2C_SMBUS_WRITE, command,
	                    I2C_SMBUS_I2C_BLOCK_DATA, &data) < 0) {
		printf("i2c: block write failed (addr 0x%02x, reg 0x%02x, len %d): %s\n",
		       i2c_address, command, length, strerror(errno));
		return -1;
	}
	return 0;
}

// This executes the SMBus "block read" protocol, returning negative errno else the number
// of data bytes in the slave's response.
int32_t i2c_read_i2c_block_data(uint8_t i2c_address, uint8_t command, uint8_t length,
        uint8_t* values) {
	if (length == 0 || length > I2C_SMBUS_BLOCK_MAX) {
		printf("i2c: block read length %d out of range\n", length);
		return -1;
	}

	union i2c_smbus_data data;
	data.block[0] = length;

	if (i2c_smbus_xfer(i2c_address, I2C_SMBUS_READ, command,
	                    I2C_SMBUS_I2C_BLOCK_DATA, &data) < 0) {
		printf("i2c: block read failed (addr 0x%02x, reg 0x%02x, len %d): %s\n",
		       i2c_address, command, length, strerror(errno));
		return -1;
	}

	uint8_t got = data.block[0];
	if (got > length)
		got = length;
	memcpy(values, &data.block[1], got);
	return got;
}
