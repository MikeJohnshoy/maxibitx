// i2c.c
//
// This code was originally derived from Marek Wyborski's i2cbb.c
// and used to bit-bang I2C over GPIO.  We now use the kernel's own
// i2c subsystem and the bit-banging code is gone - so we
// dropped the 'bb' from 'i2cbb.c'

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

// Longest register write this wrapper will build: the command byte plus
// its data. 32 matches the SMBus block limit the bit-banged code worked
// to, which is more than anything on this board sends.
#define I2C_MAX_BLOCK 32

void i2c_init(int i2c_bus_number) {
	char path[32];
	snprintf(path, sizeof(path), "/dev/i2c-%d", i2c_bus_number);

	i2c_fd = open(path, O_RDWR);
	if (i2c_fd < 0) {
		printf("i2c: failed to open %s: %s - I2C calls will fail\n",
		       path, strerror(errno));
		return;
	}

	// Every transfer below is an I2C_RDWR message list, so the adapter
	// has to offer plain I2C rather than SMBus emulation only. Checked
	// once here so a bus that can't do it says so at startup instead of
	// failing on every call.
	unsigned long funcs = 0;
	if (ioctl(i2c_fd, I2C_FUNCS, &funcs) < 0) {
		printf("i2c: cannot query %s capabilities: %s\n", path, strerror(errno));
	} else if (!(funcs & I2C_FUNC_I2C)) {
		printf("i2c: %s does not support plain I2C transfers - I2C calls will fail\n",
		       path);
	}
}

// Runs one complete transfer as a single I2C_RDWR ioctl.
//
// Deliberately NOT the I2C_SLAVE-then-I2C_SMBUS pair the usual examples
// show. That sets the target address as separate state on the shared fd,
// which is a race with more than one thread on the bus: radio_tune_to()
// reaches si5351v2.c from the rigctld, CAT and HPSDR threads while
// radio.c's TX worker does the same on every T/R transition, so one
// thread can change the address between another thread's two ioctls and
// send its transfer to the wrong device. Nothing mis-addresses while all
// runtime traffic goes to the si5351, but that ends as soon as anything
// reads the power/SWR bridge at 0x8 during transmit
// (dsp_design_notes/tx_test_tones_and_alc.md, step 5).
//
// An I2C_RDWR message carries its own address, so the whole exchange is
// indivisible in one ioctl and needs no lock at all - better than the
// mutex this replaced, which would have been a lock two threads could
// queue on.
static int i2c_transfer(struct i2c_msg *msgs, int nmsgs) {
	if (i2c_fd < 0)
		return -1;

	struct i2c_rdwr_ioctl_data xfer;
	xfer.msgs = msgs;
	xfer.nmsgs = nmsgs;

	if (ioctl(i2c_fd, I2C_RDWR, &xfer) < 0)
		return -1;
	return 0;
}

// This executes the SMBus "write byte" protocol, returning negative errno else zero on success.
int32_t i2c_write_byte_data(uint8_t i2c_address, uint8_t command, uint8_t value) {
	uint8_t buf[2] = { command, value };
	struct i2c_msg msg = { .addr = i2c_address, .flags = 0, .len = 2, .buf = buf };

	if (i2c_transfer(&msg, 1) < 0) {
		printf("i2c: write byte failed (addr 0x%02x, reg 0x%02x): %s\n",
		       i2c_address, command, strerror(errno));
		return -1;
	}
	return 0;
}

// This executes the SMBus "read byte" protocol, returning negative errno else a data byte received from the device.
int32_t i2c_read_byte_data(uint8_t i2c_address, uint8_t command) {
	uint8_t reg = command;
	uint8_t value = 0;
	// Write the register, repeated start, then read - both halves in one
	// ioctl so nothing can address another device in between.
	struct i2c_msg msgs[2] = {
		{ .addr = i2c_address, .flags = 0,        .len = 1, .buf = &reg },
		{ .addr = i2c_address, .flags = I2C_M_RD, .len = 1, .buf = &value },
	};

	if (i2c_transfer(msgs, 2) < 0) {
		printf("i2c: read byte failed (addr 0x%02x, reg 0x%02x): %s\n",
		       i2c_address, command, strerror(errno));
		return -1;
	}
	return value & 0xFF;
}

// This executes the SMBus "block write" protocol, returning negative errno else zero on success.
int32_t i2c_write_i2c_block_data(uint8_t i2c_address, uint8_t command, uint8_t length,
        const uint8_t * values) {
	if (length > I2C_MAX_BLOCK) {
		printf("i2c: block write length %d exceeds max %d\n", length, I2C_MAX_BLOCK);
		return -1;
	}

	// length 0 is "point the register pointer, no data" - the command
	// byte on its own, which is how the INA260 reads are set up.
	uint8_t buf[1 + I2C_MAX_BLOCK];
	buf[0] = command;
	if (length)
		memcpy(&buf[1], values, length);

	struct i2c_msg msg = { .addr = i2c_address, .flags = 0,
	                       .len = (uint16_t)(1 + length), .buf = buf };

	if (i2c_transfer(&msg, 1) < 0) {
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
	if (length == 0 || length > I2C_MAX_BLOCK) {
		printf("i2c: block read length %d out of range\n", length);
		return -1;
	}

	uint8_t reg = command;
	struct i2c_msg msgs[2] = {
		{ .addr = i2c_address, .flags = 0,        .len = 1,      .buf = &reg },
		{ .addr = i2c_address, .flags = I2C_M_RD, .len = length, .buf = values },
	};

	if (i2c_transfer(msgs, 2) < 0) {
		printf("i2c: block read failed (addr 0x%02x, reg 0x%02x, len %d): %s\n",
		       i2c_address, command, length, strerror(errno));
		return -1;
	}
	// A plain I2C read transfers exactly what was asked for or fails, so
	// there is no device-reported count to clamp against - unlike the
	// SMBus block protocol, which puts a length byte on the wire.
	return length;
}
