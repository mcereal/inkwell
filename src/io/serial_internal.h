#pragma once

#include "inkwell/io/serial.h"

/* The macOS scan, over the I/O Registry. Only serial.c calls it: inkwell_serial_scan() is the
   one entry point, and it decides which tree to read. */
size_t inkwell_serial_scan_iokit(struct inkwell_serial_port_info *out, size_t capacity);
