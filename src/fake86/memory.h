#pragma once
#include <stdint.h>

extern uint8_t RAM[0x100000];
extern uint8_t readonly[0x100000];

void write86(uint32_t addr32, uint8_t value);
void writew86(uint32_t addr32, uint16_t value);

uint8_t read86(uint32_t addr32);
uint16_t readw86(uint32_t addr32);

uint32_t mem_loadbinary(uint32_t addr32, uint8_t *filename, uint8_t roflag);
uint32_t mem_loadrom(uint32_t addr32, uint8_t *filename, uint8_t failure_fatal);
uint32_t mem_loadbios(uint8_t *filename);