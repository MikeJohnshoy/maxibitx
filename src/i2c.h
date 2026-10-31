// i2c.h
//
// Kernel-backed I2C access via /dev/i2c-N. Every transfer is one I2C_RDWR
// ioctl, so threads can share the bus with no lock (i2c.c).
// Each call returns -1 on failure, after printing why.

#include <stdint.h>

// Opens the given Linux I2C bus (the number after "i2c-" as shown by
// `i2cdetect -l`, e.g. 22) via the kernel's i2c-dev interface.
void i2c_init(int i2c_bus_number);

// Writes value to register command: the SMBus "write byte" framing.
// Returns 0.
int32_t i2c_write_byte_data(uint8_t i2c_address, uint8_t command, uint8_t value);

// Reads register command (write, repeated start, read): the SMBus "read
// byte" framing. Returns the byte, 0-255.
int32_t i2c_read_byte_data(uint8_t i2c_address, uint8_t command);

// Writes the command byte then length bytes (0-32; 0 just points the
// device at a register). Plain I2C, with no SMBus length byte. Returns 0.
int32_t i2c_write_i2c_block_data (uint8_t i2c_address, uint8_t command, uint8_t length,
	const uint8_t * values);

// Writes the command byte, then reads length bytes (1-32) after a
// repeated start. Plain I2C, with no SMBus length byte. Returns length.
int32_t i2c_read_i2c_block_data (uint8_t i2c_address, uint8_t command, uint8_t length, uint8_t* values);
