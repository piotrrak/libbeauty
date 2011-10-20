/*
 *  Copyright (C) 2004-2009 The libbeauty Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
/* Intel ia32 instruction format: -
 Instruction-Prefixes (Up to four prefixes of 1-byte each. [optional] )
 Opcode (1-, 2-, or 3-byte opcode)
 ModR/M (1 byte [if required] )
 SIB (Scale-Index-Base:1 byte [if required] )
 Displacement (Address displacement of 1, 2, or 4 bytes or none)
 Immediate (Immediate data of 1, 2, or 4 bytes or none)

 Naming convention taken from Intel Instruction set manual, Appendix A. 25366713.pdf
*/
#include <stdlib.h>
#include <dis.h>
#include "internal.h"

/* Little endian */
uint32_t getbyte(uint8_t *base_address, uint64_t offset) {
	uint32_t result;
	result=base_address[offset];
	printf(" 0x%x\n",result);
	return result;
}

uint32_t getword(uint8_t *base_address, uint64_t offset) {
	uint32_t result;
	result=getbyte(base_address, offset);
	offset++;
	result=result | getbyte(base_address, offset) << 8;
	offset++;
	return result;
}

uint32_t getdword(uint8_t *base_address, uint64_t offset) {
	uint32_t result;
	result=getbyte(base_address, offset);
	offset++;
	result=result | getbyte(base_address, offset) << 8;
	offset++;
	result=result | getbyte(base_address, offset) << 16;
	offset++;
	result=result | getbyte(base_address, offset) << 24;
	offset++;
	return result;
}

uint32_t relocated_code(struct rev_eng *handle, uint8_t *base_address, uint64_t offset, uint64_t size, struct reloc_table **reloc_table_entry) {
	int n;
	for (n = 0; n < handle->reloc_table_code_sz; n++) {
		if (handle->reloc_table_code[n].address == offset) {
			*reloc_table_entry = &(handle->reloc_table_code[n]);
			return 0;
		}
	}
	return 1;
}

/* REX Volume2A Section 2.2.1.2 */
void split_ModRM(uint8_t byte, uint8_t rex, uint8_t *reg,  uint8_t *reg_mem, uint8_t *mod) {
	*reg = (byte >> 3) & 0x7; //bits 3-5
	*reg_mem = (byte & 0x7); //bits 0-2
	/* mod: 00, 01, 10 = various memory addressing modes. */
	/* mod: 11 = register */
	*mod = (byte >> 6); //bit 6-7
	if (rex & 0x4) {
		*reg = *reg | 0x8;
	}
	/* Only is REX.X == 0 */
	if ((rex & 0x3) == 1) {
		*reg_mem = *reg_mem | 0x8;
	}
	printf("byte=%02x, reg=%02x, reg_mem=%02x, mod=%02x\n",
		byte,
		*reg,
		*reg_mem,
		*mod);
	}

/* REX Volume2A Section 2.2.1.2 */
void split_SIB(uint8_t byte, uint8_t rex, uint8_t *mul,  uint8_t *index, uint8_t *base) {
	*index = (byte >> 3) & 0x7; //bits 3-5
	*base = (byte & 0x7); //bits 0-2
	*mul = (byte >> 6); //bit 6-7
	if (rex & 0x2) {
		*index = *index | 0x8;
	}
	if (rex & 0x1) {
		*base = *base | 0x8;
	}

	// do the *2 etc. later
	//  *mul = 1 << *mul; // convert bits to *1, *2, *4, *8
	printf("byte=%02x, mul=%02x, index=%02x, base=%02x\n",
		byte,
		*mul,
		*index,
		*base);
}

/* Refer to Intel reference: Volume 2A, section 2.1.3 Mod R/M and SIB Bytes */
int rmb(struct rev_eng *handle, struct dis_instructions_s *dis_instructions, uint8_t *base_address, uint64_t offset, uint64_t size, uint8_t rex, uint8_t *return_reg, int *half) {
	uint8_t reg;
	uint8_t reg_mem;
	uint8_t mod;
	uint8_t mul, index, base;
	instruction_t *instruction;
	int	tmp;
	int	result = 0;
	struct reloc_table *reloc_table_entry;
	/* Does not always start at zero.
	 * e.g. 0xff 0x71 0xfd pushl -0x4(%ecx)
	 * inserts an RTL dis_instructions before calling here
	 */
	int number = dis_instructions->instruction_number;
	*half = 0;
	split_ModRM(getbyte(base_address, offset + dis_instructions->bytes_used), rex, &reg, &reg_mem, &mod);
	dis_instructions->bytes_used++;
	*return_reg = reg;
	switch (mod) {
	case 0:
		/* Special case uses SIB, when using ESP or R12 */
		if ((4 == reg_mem) || (rex & 0x02) || (0xc == reg_mem)) {
			printf("Doing SIB\n");
			split_SIB(getbyte(base_address, offset + dis_instructions->bytes_used), rex, &mul, &index, &base);
			dis_instructions->bytes_used++;
			/* FIXME: index == 4 not explicitly handled */
			if (index != 4) {
				/* Handle scaled index */
				instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
				instruction->opcode = MOV;
				instruction->flags = 0;
				instruction->srcA.store = STORE_REG;
				instruction->srcA.indirect = IND_DIRECT;
				instruction->srcA.indirect_size = 8;
				instruction->srcA.index = reg_table[index].offset;
				instruction->srcA.relocated = 0;
				instruction->srcA.value_size = reg_table[index].size;
				printf("Got here1\n");
				instruction->dstA.store = STORE_REG;
				instruction->dstA.indirect = IND_DIRECT;
				instruction->dstA.indirect_size = 8;
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 8;
				dis_instructions->instruction_number++;
				/* Skip * 1 */
				if (mul > 0) {
					instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
					instruction->opcode = MUL;
					instruction->flags = 0;
					instruction->srcA.store = STORE_DIRECT;
					instruction->srcA.indirect = IND_DIRECT;
					instruction->srcA.indirect_size = 8;
					instruction->srcA.index = 1 << mul;
					instruction->srcA.relocated = 0;
					instruction->srcA.value_size = size;
					instruction->dstA.store = STORE_REG;
					instruction->dstA.indirect = IND_DIRECT;
					instruction->dstA.indirect_size = 8;
					instruction->dstA.index = REG_TMP1;
					instruction->dstA.relocated = 0;
					instruction->dstA.value_size = 8;
					dis_instructions->instruction_number++;
				}
			}
			if (base == 5) {
				/* Handle base==5 */
				/* from Volume2A Table 2-3 Note 1 */
				instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
				if (dis_instructions->instruction_number > number) {
					instruction->opcode = ADD;
					instruction->flags = 0;
				} else {
					instruction->opcode = MOV;
					instruction->flags = 0;
				}
				instruction->srcA.store = STORE_REG;
				instruction->srcA.indirect = IND_DIRECT;
				instruction->srcA.indirect_size = 8;
				instruction->srcA.index = REG_BP;
				instruction->srcA.relocated = 0;
				instruction->srcA.value_size = size;
				instruction->dstA.store = STORE_REG;
				instruction->dstA.indirect = IND_DIRECT;
				instruction->dstA.indirect_size = 8;
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 8;
				dis_instructions->instruction_number++;

				instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
				if (dis_instructions->instruction_number > number) {
					instruction->opcode = ADD;
					instruction->flags = 0; /* Do not effect flags, just calculated indirect memory location */
				} else {
					instruction->opcode = MOV;
					instruction->flags = 0;
				}
				instruction->srcA.store = STORE_DIRECT;
				instruction->srcA.indirect = IND_DIRECT;
				instruction->srcA.indirect_size = 8;
				/* mod 1 */
				printf("Got here4 size = 1\n");
				instruction->srcA.value_size = 8;
				instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
				instruction->srcA.relocated = 0;
				printf("Got here4 byte = 0x%"PRIx64"\n", instruction->srcA.index);
				/* if the offset is negative,
				 * replace ADD with SUB */
				if ((instruction->opcode == ADD) &&
					(instruction->srcA.index > 0x7fffffff)) {
					instruction->opcode = SUB;
					tmp = 0x100000000 - instruction->srcA.index;
					instruction->srcA.index = tmp;
				}
				dis_instructions->bytes_used+=4;
				instruction->dstA.store = STORE_REG;
				instruction->dstA.indirect = IND_DIRECT;
				instruction->dstA.indirect_size = 8;
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 8;
				dis_instructions->instruction_number++;
			} else {
				instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
				if (dis_instructions->instruction_number > number) {
					instruction->opcode = ADD;
					instruction->flags = 0;
				} else {
					instruction->opcode = MOV;
					instruction->flags = 0;
				}
				instruction->srcA.store = STORE_REG;
				instruction->srcA.indirect = IND_DIRECT;
				instruction->srcA.indirect_size = 8;
				instruction->srcA.index = reg_table[base].offset;
				instruction->srcA.relocated = 0;
				instruction->srcA.value_size = reg_table[base].size;
				printf("Got here2\n");
				instruction->dstA.store = STORE_REG;
				instruction->dstA.indirect = IND_DIRECT;
				instruction->dstA.indirect_size = 8;
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 8;
				dis_instructions->instruction_number++;
			}

			result = 1;
		} else if (reg_mem == 5) {
			instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
			if (dis_instructions->instruction_number > number) {
				instruction->opcode = ADD;
				instruction->flags = 0; /* Do not effect flags, just calculated indirect memory location */
			} else {
				instruction->opcode = MOV;
				instruction->flags = 0;
			}
			instruction->srcA.store = STORE_DIRECT;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			/* mod 1 */
			printf("Got here4 size = 1\n");
			instruction->srcA.value_size = 8;
			instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
			instruction->srcA.relocated = 0;
			printf("Got here4 byte = 0x%"PRIx64"\n", instruction->srcA.index);
			/* if the offset is negative,
			 * replace ADD with SUB */
			if ((instruction->opcode == ADD) &&
				(instruction->srcA.index > 0x7fffffff)) {
				instruction->opcode = SUB;
				tmp = 0x100000000 - instruction->srcA.index;
				instruction->srcA.index = tmp;
			}
			dis_instructions->bytes_used+=4;
			instruction->dstA.store = STORE_REG;
			instruction->dstA.indirect = IND_DIRECT;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 8;
			dis_instructions->instruction_number++;

			result = 1;
		} else {
			/* Not SIB */
			printf("MODRM mod 1 number=0x%x\n", number);
			instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
			if (dis_instructions->instruction_number > number) {
				instruction->opcode = ADD;
				instruction->flags = 0;
			} else {
				instruction->opcode = MOV;
				instruction->flags = 0;
			}
			instruction->srcA.store = STORE_REG;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.index = reg_table[reg_mem].offset;
			instruction->srcA.relocated = 0;
			//instruction->srcA.value_size = reg_table[reg_mem].size;
			instruction->srcA.value_size = 8;
			printf("Got here3 size = %d\n", instruction->srcA.value_size);
			instruction->dstA.store = STORE_REG;
			instruction->dstA.indirect = IND_DIRECT;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 8;
			dis_instructions->instruction_number++;

			result = 1;
		}
		break;
	case 1:
		/* Special case uses SIB */
		if ((4 == reg_mem) || (rex & 0x02) || (0xc == reg_mem)) {
			printf("Doing SIB\n");
			split_SIB(getbyte(base_address, offset + dis_instructions->bytes_used), rex, &mul, &index, &base);
			dis_instructions->bytes_used++;
			/* FIXME: index == 4 not explicitly handled */
			if (index != 4) {
				/* Handle scaled index */
				instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
				instruction->opcode = MOV;
				instruction->flags = 0;
				instruction->srcA.store = STORE_REG;
				instruction->srcA.indirect = IND_DIRECT;
				instruction->srcA.indirect_size = 8;
				instruction->srcA.index = reg_table[index].offset;
				instruction->srcA.relocated = 0;
				instruction->srcA.value_size = reg_table[index].size;
				printf("Got here1\n");
				instruction->dstA.store = STORE_REG;
				instruction->dstA.indirect = IND_DIRECT;
				instruction->dstA.indirect_size = 8;
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 8;
				dis_instructions->instruction_number++;
				/* Skip * 1 */
				if (mul > 0) {
					instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
					instruction->opcode = MUL;
					instruction->flags = 0;
					instruction->srcA.store = STORE_DIRECT;
					instruction->srcA.indirect = IND_DIRECT;
					instruction->srcA.indirect_size = 8;
					instruction->srcA.index = 1 << mul;
					instruction->srcA.relocated = 0;
					instruction->srcA.value_size = size;
					instruction->dstA.store = STORE_REG;
					instruction->dstA.indirect = IND_DIRECT;
					instruction->dstA.indirect_size = 8;
					instruction->dstA.index = REG_TMP1;
					instruction->dstA.relocated = 0;
					instruction->dstA.value_size = 8;
					dis_instructions->instruction_number++;
				}
			}
			if (base == 5) {
				/* Handle base==5 */
				instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
				if (dis_instructions->instruction_number > number) {
					instruction->opcode = ADD;
					instruction->flags = 0;
				} else {
					instruction->opcode = MOV;
					instruction->flags = 0;
				}
				instruction->srcA.store = STORE_REG;
				instruction->srcA.indirect = IND_DIRECT;
				instruction->srcA.indirect_size = 8;
				instruction->srcA.index = REG_BP;
				instruction->srcA.relocated = 0;
				instruction->srcA.value_size = size;
				instruction->dstA.store = STORE_REG;
				instruction->dstA.indirect = IND_DIRECT;
				instruction->dstA.indirect_size = 8;
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 8;
				dis_instructions->instruction_number++;
			} else {
				instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
				if (dis_instructions->instruction_number > number) {
					instruction->opcode = ADD;
					instruction->flags = 0;
				} else {
					instruction->opcode = MOV;
					instruction->flags = 0;
				}
				instruction->srcA.store = STORE_REG;
				instruction->srcA.indirect = IND_DIRECT;
				instruction->srcA.indirect_size = 8;
				instruction->srcA.index = reg_table[base].offset;
				instruction->srcA.relocated = 0;
				instruction->srcA.value_size = reg_table[base].size;
				printf("Got here2\n");
				instruction->dstA.store = STORE_REG;
				instruction->dstA.indirect = IND_DIRECT;
				instruction->dstA.indirect_size = 8;
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 8;
				dis_instructions->instruction_number++;
			}
			instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
			if (dis_instructions->instruction_number > number) {
				instruction->opcode = ADD;
				instruction->flags = 0; /* Do not effect flags, just calculated indirect memory location */
			} else {
				instruction->opcode = MOV;
				instruction->flags = 0;
			}
			instruction->srcA.store = STORE_DIRECT;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			/* mod 1 */
			printf("Got here4 size = 1\n");
			instruction->srcA.value_size = 8;
			instruction->srcA.index = getbyte(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
			instruction->srcA.relocated = 0;
			printf("Got here4 byte = 0x%"PRIx64"\n", instruction->srcA.index);
			/* if the offset is negative,
			 * replace ADD with SUB */
			if ((instruction->opcode == ADD) &&
				(instruction->srcA.index > 0x7f)) {
				instruction->opcode = SUB;
				tmp = 0x100 - instruction->srcA.index;
				instruction->srcA.index = tmp;
			}
			dis_instructions->bytes_used++;
			instruction->dstA.store = STORE_REG;
			instruction->dstA.indirect = IND_DIRECT;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 8;
			dis_instructions->instruction_number++;
			result = 1;
		} else {
			/* Not SIB */
			printf("MODRM mod 1 number=0x%x\n", number);
			instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
			if (dis_instructions->instruction_number > number) {
				instruction->opcode = ADD;
				instruction->flags = 0;
			} else {
				instruction->opcode = MOV;
				instruction->flags = 0;
			}
			instruction->srcA.store = STORE_REG;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.index = reg_table[reg_mem].offset;
			instruction->srcA.relocated = 0;
			//instruction->srcA.value_size = reg_table[reg_mem].size;
			instruction->srcA.value_size = 8;
			printf("Got here3 size = %d\n", instruction->srcA.value_size);
			instruction->dstA.store = STORE_REG;
			instruction->dstA.indirect = IND_DIRECT;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 8;
			dis_instructions->instruction_number++;
			instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
			if (dis_instructions->instruction_number > number) {
				instruction->opcode = ADD;
				instruction->flags = 0; /* Do not effect flags, just calculated indirect memory location */
			} else {
				instruction->opcode = MOV;
				instruction->flags = 0;
			}
			instruction->srcA.store = STORE_DIRECT;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			/* mod 1 */
			printf("Got here4 size = 1\n");
			instruction->srcA.value_size = 8;
			instruction->srcA.index = getbyte(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
			instruction->srcA.relocated = 0;
			printf("Got here4 byte = 0x%"PRIx64"\n", instruction->srcA.index);
			/* if the offset is negative,
			 * replace ADD with SUB */
			if ((instruction->opcode == ADD) &&
				(instruction->srcA.index > 0x7f)) {
				instruction->opcode = SUB;
				tmp = 0x100 - instruction->srcA.index;
				instruction->srcA.index = tmp;
			}
			dis_instructions->bytes_used++;
			instruction->dstA.store = STORE_REG;
			instruction->dstA.indirect = IND_DIRECT;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 8;
			dis_instructions->instruction_number++;

			result = 1;
		}
		break;
	case 2:
		/* Special case uses SIB */
		if ((4 == reg_mem) || (rex & 0x02) || (0xc == reg_mem)) {
			printf("Doing SIB\n");
			split_SIB(getbyte(base_address, offset + dis_instructions->bytes_used), rex, &mul, &index, &base);
			dis_instructions->bytes_used++;
			/* FIXME: index == 4 not explicitly handled */
			if (index != 4) {
				/* Handle scaled index */
				instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
				instruction->opcode = MOV;
				instruction->flags = 0;
				instruction->srcA.store = STORE_REG;
				instruction->srcA.indirect = IND_DIRECT;
				instruction->srcA.indirect_size = 8;
				instruction->srcA.index = reg_table[index].offset;
				instruction->srcA.relocated = 0;
				instruction->srcA.value_size = reg_table[index].size;
				printf("Got here1\n");
				instruction->dstA.store = STORE_REG;
				instruction->dstA.indirect = IND_DIRECT;
				instruction->dstA.indirect_size = 8;
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 8;
				dis_instructions->instruction_number++;
				/* Skip * 1 */
				if (mul > 0) {
					instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
					instruction->opcode = MUL;
					instruction->flags = 0;
					instruction->srcA.store = STORE_DIRECT;
					instruction->srcA.indirect = IND_DIRECT;
					instruction->srcA.indirect_size = 8;
					instruction->srcA.index = 1 << mul;
					instruction->srcA.relocated = 0;
					instruction->srcA.value_size = size;
					instruction->dstA.store = STORE_REG;
					instruction->dstA.indirect = IND_DIRECT;
					instruction->dstA.indirect_size = 8;
					instruction->dstA.index = REG_TMP1;
					instruction->dstA.relocated = 0;
					instruction->dstA.value_size = 8;
					dis_instructions->instruction_number++;
				}
			}
			if (base == 5) {
				/* Handle base==5 */
				instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
				if (dis_instructions->instruction_number > number) {
					instruction->opcode = ADD;
					instruction->flags = 0;
				} else {
					instruction->opcode = MOV;
					instruction->flags = 0;
				}
				instruction->srcA.store = STORE_REG;
				instruction->srcA.indirect = IND_DIRECT;
				instruction->srcA.indirect_size = 8;
				instruction->srcA.index = REG_BP;
				instruction->srcA.relocated = 0;
				instruction->srcA.value_size = size;
				instruction->dstA.store = STORE_REG;
				instruction->dstA.indirect = IND_DIRECT;
				instruction->dstA.indirect_size = 8;
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 8;
				dis_instructions->instruction_number++;
			} else {
				instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
				if (dis_instructions->instruction_number > number) {
					instruction->opcode = ADD;
					instruction->flags = 0;
				} else {
					instruction->opcode = MOV;
					instruction->flags = 0;
				}
				instruction->srcA.store = STORE_REG;
				instruction->srcA.indirect = IND_DIRECT;
				instruction->srcA.indirect_size = 8;
				instruction->srcA.index = reg_table[base].offset;
				instruction->srcA.relocated = 0;
				instruction->srcA.value_size = reg_table[base].size;
				printf("Got here2\n");
				instruction->dstA.store = STORE_REG;
				instruction->dstA.indirect = IND_DIRECT;
				instruction->dstA.indirect_size = 8;
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 8;
				dis_instructions->instruction_number++;
			}
			instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
			if (dis_instructions->instruction_number > number) {
				instruction->opcode = ADD;
				instruction->flags = 0; /* Do not effect flags, just calculated indirect memory location */
			} else {
				instruction->opcode = MOV;
				instruction->flags = 0;
			}
			instruction->srcA.store = STORE_DIRECT;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			/* mod 1 */
			printf("Got here4 size = 1\n");
			instruction->srcA.value_size = 8;
			instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
			instruction->srcA.relocated = 0;
			printf("Got here4 byte = 0x%"PRIx64"\n", instruction->srcA.index);
			/* if the offset is negative,
			 * replace ADD with SUB */
			if ((instruction->opcode == ADD) &&
				(instruction->srcA.index > 0x7fffffff)) {
				instruction->opcode = SUB;
				tmp = 0x100000000 - instruction->srcA.index;
				instruction->srcA.index = tmp;
			}
			dis_instructions->bytes_used+=4;
			instruction->dstA.store = STORE_REG;
			instruction->dstA.indirect = IND_DIRECT;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 8;
			dis_instructions->instruction_number++;

			result = 1;
		} else {
			/* Not SIB */
			printf("MODRM mod 1 number=0x%x\n", number);
			instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
			if (dis_instructions->instruction_number > number) {
				instruction->opcode = ADD;
				instruction->flags = 0;
			} else {
				instruction->opcode = MOV;
				instruction->flags = 0;
			}
			instruction->srcA.store = STORE_REG;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.index = reg_table[reg_mem].offset;
			instruction->srcA.relocated = 0;
			//instruction->srcA.value_size = reg_table[reg_mem].size;
			instruction->srcA.value_size = 8;
			printf("Got here3 size = %d\n", instruction->srcA.value_size);
			instruction->dstA.store = STORE_REG;
			instruction->dstA.indirect = IND_DIRECT;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 8;
			dis_instructions->instruction_number++;
			instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
			if (dis_instructions->instruction_number > number) {
				instruction->opcode = ADD;
				instruction->flags = 0; /* Do not effect flags, just calculated indirect memory location */
			} else {
				instruction->opcode = MOV;
				instruction->flags = 0;
			}
			instruction->srcA.store = STORE_DIRECT;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			/* mod 1 */
			printf("Got here4 size = 1\n");
			instruction->srcA.value_size = 8;
			instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
			instruction->srcA.relocated = 0;
			printf("Got here4 byte = 0x%"PRIx64"\n", instruction->srcA.index);
			/* if the offset is negative,
			 * replace ADD with SUB */
			if ((instruction->opcode == ADD) &&
				(instruction->srcA.index > 0x7fffffff)) {
				instruction->opcode = SUB;
				tmp = 0x100000000 - instruction->srcA.index;
				instruction->srcA.index = tmp;
			}
			dis_instructions->bytes_used+=4;
			instruction->dstA.store = STORE_REG;
			instruction->dstA.indirect = IND_DIRECT;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 8;
			dis_instructions->instruction_number++;

			result = 1;
		}
		break;
	case 3:  // Special case Reg to Reg transfer
		/* Fill in half of instruction */
		printf("mod 3\n");
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = NOP;
		instruction->flags = 0;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = reg_table[reg_mem].offset;
		instruction->srcA.relocated = 0;
		instruction->srcA.value_size = size;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = reg_table[reg_mem].offset;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = size;
		*half = 1;
		result = 1;
		break;
	}
#if 0
	case 2:
	/* FIXME: Case where mod == 3 is not handled yet */
		if (reg_mem == 4) {
			split_SIB(getbyte(base_address, offset + dis_instructions->bytes_used), rex, &mul, &index, &base);
			dis_instructions->bytes_used++;
			/* FIXME: index == 4 not explicitly handled */
			if (index != 4) {
				/* Handle scaled index */
				instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
				instruction->opcode = MOV;
				instruction->flags = 0;
				instruction->srcA.store = STORE_REG;
				instruction->srcA.indirect = IND_DIRECT;
				instruction->srcA.indirect_size = 8;
				instruction->srcA.index = reg_table[index].offset;
				instruction->srcA.relocated = 0;
				instruction->srcA.value_size = reg_table[index].size;
				printf("Got here1\n");
				instruction->dstA.store = STORE_REG;
				instruction->dstA.indirect = IND_DIRECT;
				instruction->dstA.indirect_size = 8;
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 8;
				dis_instructions->instruction_number++;
				/* Skip * 1 */
				if (mul > 0) {
					instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
					instruction->opcode = MUL;
					instruction->flags = 0;
					instruction->srcA.store = STORE_DIRECT;
					instruction->srcA.indirect = IND_DIRECT;
					instruction->srcA.indirect_size = 8;
					instruction->srcA.index = 1 << mul;
					instruction->srcA.relocated = 0;
					instruction->srcA.value_size = size;
					instruction->dstA.store = STORE_REG;
					instruction->dstA.indirect = IND_DIRECT;
					instruction->dstA.indirect_size = 8;
					instruction->dstA.index = REG_TMP1;
					instruction->dstA.relocated = 0;
					instruction->dstA.value_size = 8;
					dis_instructions->instruction_number++;
				}
			}
			if (base == 5) {
			/* Handle base==5 */
			instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
			if (dis_instructions->instruction_number > number) {
				instruction->opcode = ADD;
				instruction->flags = 0;
			} else {
				instruction->opcode = MOV;
				instruction->flags = 0;
			}
/*
			if (mod==0) {
				instruction->srcA.store = STORE_DIRECT;
				instruction->srcA.indirect = IND_DIRECT;
				instruction->srcA.indirect_size = 8;
				instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
				instruction->srcA.relocated = 0;
				tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
				if (!tmp) {
					instruction->srcA.relocated = 1;
				}
				dis_instructions->bytes_used+=4;
				instruction->srcA.value_size = 4;
*/
			instruction->srcA.store = STORE_REG;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.index = REG_BP;
			instruction->srcA.relocated = 0;
			instruction->srcA.value_size = size;
			instruction->dstA.store = STORE_REG;
			instruction->dstA.indirect = IND_DIRECT;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 8;
			dis_instructions->instruction_number++;
		} else {
			instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
			if (dis_instructions->instruction_number > number) {
				instruction->opcode = ADD;
				instruction->flags = 0;
			} else {
				instruction->opcode = MOV;
				instruction->flags = 0;
			}
			instruction->srcA.store = STORE_REG;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.index = reg_table[base].offset;
			instruction->srcA.relocated = 0;
			instruction->srcA.value_size = reg_table[base].size;
			printf("Got here2\n");
			instruction->dstA.store = STORE_REG;
			instruction->dstA.indirect = IND_DIRECT;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 8;
			dis_instructions->instruction_number++;
		}
	} else if ((reg_mem == 5) && (mod == 0)) {
		/* FIXME: Cannot handle 48 8b 05 00 00 00 00 */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		if (dis_instructions->instruction_number > number) {
			instruction->opcode = ADD;
			instruction->flags = 0;
		} else {
			instruction->opcode = MOV;
			instruction->flags = 0;
		}
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		instruction->srcA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			/* FIXME: what about relocations that use a different howto? */
			printf("reg_mem=5, mod=0, relocation table entry exists. value=0x%"PRIx64"\n", reloc_table_entry->value);
			instruction->srcA.relocated = 1;
			instruction->srcA.index = reloc_table_entry->value;
		}
		dis_instructions->bytes_used+=4;
		instruction->srcA.value_size = size;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_TMP1;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 8;
		dis_instructions->instruction_number++;
	} else {
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		if (dis_instructions->instruction_number > number) {
			instruction->opcode = ADD;
			instruction->flags = 0;
		} else {
			instruction->opcode = MOV;
			instruction->flags = 0;
		}
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = reg_table[reg_mem].offset;
		instruction->srcA.relocated = 0;
		//instruction->srcA.value_size = reg_table[reg_mem].size;
		instruction->srcA.value_size = 8;
		printf("Got here3 size = %d\n", instruction->srcA.value_size);
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_TMP1;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 8;
		dis_instructions->instruction_number++;
	}
	if (mod > 0) {
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		if (dis_instructions->instruction_number > number) {
			instruction->opcode = ADD;
			instruction->flags = 0; /* Do not effect flags, just calculated indirect memory location */
		} else {
			instruction->opcode = MOV;
			instruction->flags = 0;
		}
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		if (mod == 1) {
			printf("Got here4 size = 1\n");
			instruction->srcA.value_size = 8;
			instruction->srcA.index = getbyte(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
			instruction->srcA.relocated = 0;
			printf("Got here4 byte = 0x%"PRIx64"\n", instruction->srcA.index);
			/* if the offset is negative,
			 * replace ADD with SUB */
			if ((instruction->opcode == ADD) &&
				(instruction->srcA.index > 0x7f)) {
				instruction->opcode = SUB;
				tmp = 0x100 - instruction->srcA.index;
				instruction->srcA.index = tmp;
			}
			dis_instructions->bytes_used++;
		} else { /* mod == 2 */
			instruction->srcA.value_size = size;
			instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
			instruction->srcA.relocated = 0;
			tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
			if (!tmp) {
				instruction->srcA.relocated = 1;
			}
			dis_instructions->bytes_used+=4;
		}
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_TMP1;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 8;
		dis_instructions->instruction_number++;
	}
	return 0;
#endif
	return result;
}

int dis_Ex_Gx(struct rev_eng *handle, int opcode, uint8_t rex, struct dis_instructions_s *dis_instructions, uint8_t *base_address, uint64_t offset, uint8_t *reg, int size) {
	int half;
	int tmp;
	instruction_t *instruction;
	/* FIXME: Cannot handle 89 16 */

	tmp = rmb(handle, dis_instructions, base_address, offset, size, rex, reg, &half);
	if (!tmp) {
		return 0;
	}
	instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
	instruction->opcode = opcode;
	instruction->flags = 1;
	if (opcode == MOV) {
		instruction->flags = 0;
	}
	instruction->srcA.store = STORE_REG;
	instruction->srcA.indirect = IND_DIRECT;
	instruction->srcA.indirect_size = 8;
	instruction->srcA.index = reg_table[*reg].offset;
	instruction->srcA.relocated = 0;
	instruction->srcA.value_size = size;
	if (!half) {
		instruction->dstA.indirect = IND_MEM;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.store = STORE_REG;
		if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
		    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
			instruction->dstA.indirect = IND_STACK; /* SP and BP use STACK memory and not DATA memory. */
			instruction->dstA.indirect_size = 8;
		}
		instruction->dstA.index = REG_TMP1;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = size;
	}
	dis_instructions->instruction_number++;
	return 1;
}

int dis_Gx_Ex(struct rev_eng *handle, int opcode, uint8_t rex, struct dis_instructions_s *dis_instructions, uint8_t *base_address, uint64_t offset, uint8_t *reg, int size) {
	int half=0;
	int tmp;
	instruction_t *instruction;

	tmp = rmb(handle, dis_instructions, base_address, offset, size, rex, reg, &half);
	if (!tmp) {
		return 0;
	}
	instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
	instruction->opcode = opcode;
	instruction->flags = 1;
	instruction->dstA.store = STORE_REG;
	instruction->dstA.indirect = IND_DIRECT;
	instruction->dstA.indirect_size = 8;
	instruction->dstA.index = reg_table[*reg].offset;
	instruction->dstA.relocated = 0;
	instruction->dstA.value_size = size;
	if (!half) {
		printf("!half\n");
		instruction->srcA.indirect = IND_MEM;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.store = STORE_REG;
		if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
		    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
			instruction->srcA.indirect = IND_STACK; /* SP and BP use STACK memory and not DATA memory. */
			instruction->srcA.indirect_size = 8;
		}
		instruction->srcA.index = REG_TMP1;
		instruction->srcA.relocated = 0;
		instruction->srcA.value_size = size;
	}
	dis_instructions->instruction_number++;
	return 1;
}

int dis_Ex_Ix(struct rev_eng *handle, int opcode, uint8_t rex, struct dis_instructions_s *dis_instructions, uint8_t *base_address, uint64_t offset, uint8_t *reg, int size) {
	int half;
	int tmp;
	struct reloc_table *reloc_table_entry;
	instruction_t *instruction;

	tmp = rmb(handle, dis_instructions, base_address, offset, size, rex, reg, &half);
	if (!tmp) {
		return 0;
	}
	instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
	instruction->opcode = opcode;
	instruction->flags = 1;
	instruction->srcA.store = STORE_DIRECT;
	instruction->srcA.indirect = IND_DIRECT;
	instruction->srcA.indirect_size = 8;
	if (4 == size || 8 == size) {
		// Means get from rest of instruction
		instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used);
		instruction->srcA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		dis_instructions->bytes_used+=4;
	} else if (2 == size) {
		// Means get from rest of instruction
		instruction->srcA.index = getword(base_address, offset + dis_instructions->bytes_used);
		instruction->srcA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 2, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		dis_instructions->bytes_used+=2;
	} else if (1 == size) {
		// Means get from rest of instruction
		instruction->srcA.index = getbyte(base_address, offset + dis_instructions->bytes_used);
		instruction->srcA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 1, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		dis_instructions->bytes_used+=1;
	} else {
		printf("FIXME:JCD1\n");
		return 0;
	}
	instruction->srcA.value_size = size;
	if (!half) {
		instruction->dstA.indirect = IND_MEM;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.store = STORE_REG;
		if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
			(dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
			instruction->dstA.indirect = IND_STACK; /* SP and BP use STACK memory and not DATA memory. */
			instruction->dstA.indirect_size = 8;
		}
		instruction->dstA.index = REG_TMP1;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = size;
	}
	dis_instructions->instruction_number++;
	return 1;
}
int dis_Ex(struct rev_eng *handle, int *table, uint8_t rex, struct dis_instructions_s *dis_instructions, uint8_t *base_address, uint64_t offset, int size) {
	uint8_t reg;
	uint8_t reg_mem;
	uint8_t mod;
	int number;
	int tmp;
	int result = 0;
	struct reloc_table *reloc_table_entry;
	instruction_t *instruction;

	number = dis_instructions->instruction_number;
	split_ModRM(getbyte(base_address, offset + dis_instructions->bytes_used), rex, &reg, &reg_mem, &mod);
	dis_instructions->bytes_used++;
	switch (mod) {
	case 3:
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = table[reg]; /* FIXME is this reg right */
		instruction->flags = 1;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		if (0 == reg) {  /* Only the TEST instruction uses Ix */
			instruction->srcA.store = STORE_DIRECT;
			if (4 == size || 8 == size) {
				// Means get from rest of instruction
				instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used);
				instruction->srcA.relocated = 0;
				tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
				if (!tmp) {
					instruction->srcA.relocated = 1;
				}
				dis_instructions->bytes_used+=4;
			} else if (2 == size) {
				// Means get from rest of instruction
				instruction->srcA.index = getword(base_address, offset + dis_instructions->bytes_used);
				instruction->srcA.relocated = 0;
				tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 2, &reloc_table_entry);
				if (!tmp) {
					instruction->srcA.relocated = 1;
				}
				dis_instructions->bytes_used+=2;
			} else if (1 == size) {
				// Means get from rest of instruction
				instruction->srcA.index = getbyte(base_address, offset + dis_instructions->bytes_used);
				instruction->srcA.relocated = 0;
				tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 1, &reloc_table_entry);
				if (!tmp) {
					instruction->srcA.relocated = 1;
				}
				dis_instructions->bytes_used+=1;
			} else {
				printf("FIXME:JCD1\n");
				return 0;
			}
		} else {
			instruction->srcA.store = STORE_REG;
			instruction->srcA.index = reg_table[reg_mem].offset;
			instruction->srcA.relocated = 0;
		}
		instruction->srcA.value_size = size;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = reg_table[reg_mem].offset;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = size;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	default:
		result = 0;
		break;
	}
	return result;
};


int disassemble(struct rev_eng *handle, struct dis_instructions_s *dis_instructions, uint8_t *base_address, uint64_t offset) {
	uint8_t reg = 0;
	int half = 0;
	int result = 0;
	int8_t relative = 0;
	int64_t long_signed;
	uint8_t byte;
	int tmp;
	int8_t rel8;
	int32_t rel32;
	int64_t rel64;
	struct reloc_table *reloc_table_entry;
	uint64_t width = 4;
	uint8_t rex = 0;
	uint8_t p66 = 0;
	instruction_t *instruction;

	printf("inst[0]=0x%x\n",base_address[offset + 0]);
	dis_instructions->instruction[dis_instructions->instruction_number].opcode = NOP; /* Un-supported OPCODE */
	dis_instructions->instruction[dis_instructions->instruction_number].flags = 0; /* No flags effected */
	
	byte = getbyte(base_address, offset + dis_instructions->bytes_used);
	dis_instructions->bytes_used++;
	printf("BYTE=%02x\n", byte);
	if (byte == 0x66) {
		/* Detect Operand length prefix. */
		p66 = 1;
		width = 2;
		byte = getbyte(base_address, offset + dis_instructions->bytes_used);
		dis_instructions->bytes_used++;
		printf("BYTE=%02x\n", byte);
	}
	if (byte == 0x2e) {
		/* Detect Operand CS prefix. */
		/* FIXME: Only skips for now */
		byte = getbyte(base_address, offset + dis_instructions->bytes_used);
		dis_instructions->bytes_used++;
		printf("BYTE=%02x\n", byte);
	}

	if ((byte & 0xf0 ) == 0x40) {
		/* Detect REX. */
		rex = byte & 0xf;
		byte = getbyte(base_address, offset + dis_instructions->bytes_used);
		dis_instructions->bytes_used++;
		printf("BYTE=%02x\n", byte);
	}

	if (rex & 0x8) {
		width = 8;
	}

	switch(byte) {
	case 0x00:												/* ADD Eb,Gb */
		result = dis_Ex_Gx(handle, ADD, rex, dis_instructions, base_address, offset, &reg, 1);
		break;
	case 0x01:												/* ADD Ev,Gv */
		result = dis_Ex_Gx(handle, ADD, rex, dis_instructions, base_address, offset, &reg, width);
		break;
	case 0x02:												/* ADD Gb,Eb */
		result = dis_Gx_Ex(handle, ADD, rex, dis_instructions, base_address, offset, &reg, 1);
		break;
	case 0x03:												/* ADD Gv,Ev */
		result = dis_Gx_Ex(handle, ADD, rex, dis_instructions, base_address, offset, &reg, width);
		break;
	case 0x04:												/* ADD AL,Ib */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = ADD;
		instruction->flags = 1;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = getbyte(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		instruction->srcA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		dis_instructions->bytes_used += 1;
		instruction->srcA.value_size = 1;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_AX;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 1;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0x05:												/* ADD eAX,Iv */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = ADD;
		instruction->flags = 1;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		instruction->srcA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		dis_instructions->bytes_used+=4;
		instruction->srcA.value_size = 4;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_AX;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 4;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0x06:												/* PUSH ES */
                /* PUSH -> SP=SP-4 (-2 for word); [SP]=ES; */
		break;
	case 0x07:												/* POP ES */		
                /* POP -> ES=[SP]; SP=SP+4 (+2 for word); */
		break;
	case 0x08:												/* OR Eb,Gb */
		result = dis_Ex_Gx(handle, OR, rex, dis_instructions, base_address, offset, &reg, 1);
		break;
	case 0x09:												/* OR Ev,Gv */
		result = dis_Ex_Gx(handle, OR, rex, dis_instructions, base_address, offset, &reg, 4);
		break;
	case 0x0a:												/* OR Gb,Eb */
		result = dis_Gx_Ex(handle, OR, rex, dis_instructions, base_address, offset, &reg, 1);
		result = 1;
		break;
	case 0x0b:												/* OR Gv,Ev */
		result = dis_Gx_Ex(handle, OR, rex, dis_instructions, base_address, offset, &reg, 4);
		break;
	case 0x0c:												/* OR AL,Ib */
		break;
	case 0x0d:												/* OR eAX,Iv */
		break;
	case 0x0e:												/* PUSH CS */		
		break;
	case 0x0f:												/* 2 byte opcodes*/
		result = prefix_0f(handle, dis_instructions, base_address, offset, width, rex);
		break;
	case 0x10:												/* ADC Eb,Gb */
		break;
	case 0x11:												/* ADC Ev,Gv */
		break;
	case 0x12:												/* ADC Gb,Eb */
		break;
	case 0x13:												/* ADC Gv,Ev */
		break;
	case 0x14:												/* ADC AL,Ib */
		break;
	case 0x15:												/* ADC eAX,Iv */
		break;
	case 0x16:												/* PUSH SS */		
		break;
	case 0x17:												/* POP SS */		
		break;
	case 0x18:												/* SBB Eb,Gb */
		break;
	case 0x19:												/* SBB Ev,Gv */
		result = dis_Ex_Gx(handle, SBB, rex, dis_instructions, base_address, offset, &reg, width);
		break;
	case 0x1a:												/* SBB Gb,Eb */
		break;
	case 0x1b:												/* SBB Gv,Ev */
		break;
	case 0x1c:												/* SBB AL,Ib */
		break;
	case 0x1d:												/* SBB eAX,Iv */
		break;
	case 0x1e:												/* PUSH DS */		
		break;
	case 0x1f:												/* POP DS */
		break;
	case 0x20:												/* AND Eb,Gb */
		break;
	case 0x21:												/* AND Ev,Gv */
		result = dis_Ex_Gx(handle, rAND, rex, dis_instructions, base_address, offset, &reg, width);
		break;
	case 0x22:												/* AND Gb,Eb */
		break;
	case 0x23:												/* AND Gv,Ev */
		break;
	case 0x24:												/* AND AL,Ib */
		break;
	case 0x25:												/* AND eAX,Iv */
		break;
	case 0x26:												/* SEG ES: */
		break;
	case 0x27:												/* DAA */
		break;
	case 0x28:												/* SUB Eb,Gb */
		result = dis_Ex_Gx(handle, SUB, rex, dis_instructions, base_address, offset, &reg, 1);
		break;
	case 0x29:												/* SUB Ev,Gv */
		result = dis_Ex_Gx(handle, SUB, rex, dis_instructions, base_address, offset, &reg, width);
		break;
	case 0x2a:												/* SUB Gb,Eb */
		result = dis_Gx_Ex(handle, SUB, rex, dis_instructions, base_address, offset, &reg, 1);
		break;
	case 0x2b:												/* SUB Gv,Ev */
		result = dis_Gx_Ex(handle, SUB, rex, dis_instructions, base_address, offset, &reg, 4);
		break;
	case 0x2c:												/* SUB AL,Ib */
		break;
	case 0x2d:												/* SUB eAX,Iv */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = SUB;
		instruction->flags = 1;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		instruction->srcA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		dis_instructions->bytes_used+=4;
		instruction->srcA.value_size = 4;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_AX;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 4;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0x2e:												/* SEG CS: */
		break;
	case 0x2f:												/* DAS */
		break;
	case 0x30:												/* XOR Eb,Gb */
		break;
	case 0x31:												/* XOR Ev,Gv */
		result = dis_Ex_Gx(handle, XOR, rex, dis_instructions, base_address, offset, &reg, width);
		break;
	case 0x32:												/* XOR Gb,Eb */
		result = dis_Gx_Ex(handle, XOR, rex, dis_instructions, base_address, offset, &reg, 1);
		break;
	case 0x33:												/* XOR Gv,Ev */
		result = dis_Gx_Ex(handle, XOR, rex, dis_instructions, base_address, offset, &reg, width);
		break;
	case 0x34:												/* XOR AL,Ib */
	case 0x35:												/* XOR eAX,Iv */
	case 0x36:												/* SEG SS: */
	case 0x37:												/* AAA */
		break;
	case 0x38:												/* CMP Eb,Gb */
		result = dis_Ex_Gx(handle, CMP, rex, dis_instructions, base_address, offset, &reg, 1);
		break;
	case 0x39:												/* CMP Ev,Gv */
		result = dis_Ex_Gx(handle, CMP, rex, dis_instructions, base_address, offset, &reg, width);
		break;
	case 0x3a:												/* CMP Gb,Eb */
		result = dis_Gx_Ex(handle, CMP, rex, dis_instructions, base_address, offset, &reg, 1);
		break;
	case 0x3b:												/* CMP Gv,Ev */
		result = dis_Gx_Ex(handle, CMP, rex, dis_instructions, base_address, offset, &reg, width);
		break;
	case 0x3c:												/* CMP AL,Ib */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = CMP;
		instruction->flags = 1;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 1;
		instruction->srcA.index = getbyte(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		instruction->srcA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 1, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		dis_instructions->bytes_used+=1;
		instruction->srcA.value_size = 1;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 1;
		instruction->dstA.index = REG_AX;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0x3d:												/* CMP eAX,Iv */
		/* FIXME: Handle non standard widths */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = CMP;
		instruction->flags = 1;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 4;
		instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		instruction->srcA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		dis_instructions->bytes_used += 4;
		instruction->srcA.value_size = 4;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 4;
		instruction->dstA.index = REG_AX;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0x3e:												/* SEG DS: */
	case 0x3f:												/* AAS */
		break;
	case 0x40:												/* REX */
	case 0x41:												/*  */
	case 0x42:												/*  */
	case 0x43:												/*  */
	case 0x44:												/*  */
	case 0x45:												/*  */
	case 0x46:												/*  */
	case 0x47:												/*  */
	case 0x48:												/*  */
	case 0x49:												/*  */
	case 0x4a:												/*  */
	case 0x4b:												/*  */
	case 0x4c:												/*  */
	case 0x4d:												/*  */
	case 0x4e:												/*  */
	case 0x4f:												/*  */
		break;
/* PUSH reg. 0x50-0x57 is handled by same code. */
	case 0x50:												/* PUSH eAX */
	case 0x51:												/* PUSH CX */
	case 0x52:												/* PUSH DX */
	case 0x53:												/* PUSH BX */
	case 0x54:												/* PUSH SP */
	case 0x55:												/* PUSH BP */
	case 0x56:												/* PUSH SI */
	case 0x57:												/* PUSH DI */
                /* PUSH -> SP=SP-4 (-2 for word); [SP]=reg; */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = SUB;
		instruction->flags = 0; /* Do not effect flags */
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = 8;
		instruction->srcA.relocated = 0;
		instruction->srcA.value_size = 8;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_SP;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 8;
		dis_instructions->instruction_number++;

		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = MOV;
		instruction->flags = 0;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = reg_table[base_address[offset + 0] & 0x7].offset;
		instruction->srcA.relocated = 0;
		instruction->srcA.value_size = 8;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_STACK;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_SP;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 8;
		dis_instructions->instruction_number++;
		result = 1;
		break;
/* POP reg */
	case 0x58:												/* POP eAX */
	case 0x59:												/* POP CX */
	case 0x5a:												/* POP DX */
	case 0x5b:												/* POP BX */
	case 0x5c:												/* POP SP */
	case 0x5d:												/* POP BP */
	case 0x5e:												/* POP SI */
	case 0x5f:												/* POP DI */
                /* POP -> ES=[SP]; SP=SP+4 (+2 for word); */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = MOV;
		instruction->flags = 0;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = reg_table[base_address[offset + 0] & 0x7].offset;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 4;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_STACK;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = REG_SP;
		instruction->srcA.relocated = 0;
		instruction->srcA.value_size = 4;
		dis_instructions->instruction_number++;

		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = ADD;
		instruction->flags = 0;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = 4;
		instruction->srcA.relocated = 0;
		instruction->srcA.value_size = 4;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_SP;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 4;
		dis_instructions->instruction_number++;
		result = 1;
		break;

	case 0x60:												/* PUSHA */
		break;
	case 0x61:												/* POPA */
		break;
	case 0x62:												/* BOUND */
		break;
	case 0x63:												/* MOVS Rv,Rw */
		/* MOVSDX: Signed extention. 32 bit to 64 bit. */
		result = dis_Ex_Gx(handle, SEX, rex, dis_instructions, base_address, offset, &reg, width);
		/* Correct value_size */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number - 1];	
		instruction->srcA.value_size = width / 2;
		instruction->dstA.value_size = width;
		break;
	case 0x64:												/* SEG FS: */
		break;
	case 0x65:												/* SEG GS: */
		break;
	case 0x66:												/* Operand Size Prefix */
		break;
	case 0x67:												/* Address Size Prefix */
		break;
	case 0x68:												/* PUSH Iv */
		break;
	case 0x69:												/* IMUL Gv,Ev,Iv */
		tmp = rmb(handle, dis_instructions, base_address, offset, width, rex, &reg, &half);
		if (!tmp) {
			result = 0;
			break;
		}
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = MOV;
		instruction->flags = 0;
    		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.store = STORE_REG;
		if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
		    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
			instruction->dstA.indirect = IND_STACK; /* SP and BP use STACK memory and not DATA memory. */
			instruction->dstA.indirect_size = 8;
		}
		instruction->dstA.index = REG_TMP1;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 4;
		dis_instructions->instruction_number++;
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
		instruction->opcode = IMUL;
		instruction->flags = 1;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		/* FIXME: This may sometimes be a word and not dword */
		instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		instruction->srcA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		/* FIXME: Length wrong */
		dis_instructions->bytes_used += 4;
		instruction->srcA.value_size = width;
    		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.index = REG_TMP1;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 4;
		dis_instructions->instruction_number++;

		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
		instruction->opcode = MOV;
		instruction->flags = 0;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = REG_TMP1;
		instruction->srcA.relocated = 0;
		instruction->srcA.value_size = width;
    		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.store = STORE_REG;
		if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
		    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
			instruction->dstA.indirect = IND_STACK; /* SP and BP use STACK memory and not DATA memory. */
			instruction->dstA.indirect_size = 8;
		}
		instruction->dstA.index = reg;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = width;
		dis_instructions->instruction_number++;

		result = 1;
		break;
	case 0x6a:												/* PUSH Ib */
		break;
	case 0x6b:												/* IMUL Gv,Ev,Ib */
		break;
	case 0x6c:												/* INSB */
	case 0x6d:												/* INSW */
	case 0x6e:												/* OUTSB */
	case 0x6f:												/* OUTSW */
		break;
	case 0x70:												/* JO */
	case 0x71:												/* JNO */
	case 0x72:												/* JB */
	case 0x73:												/* JNB */
	case 0x74:												/* JZ */
	case 0x75:												/* JNZ */
	case 0x76:	/* JBE or JNA */
	case 0x77:	/* JNBE or JA */
	case 0x78:												/* JS */
	case 0x79:												/* JNS */
	case 0x7a:												/* JP */
	case 0x7b:												/* JNP */
	case 0x7c:												/* JL */
	case 0x7d:												/* JNL */
	case 0x7e:	/* JLE relative 1 byte */
	case 0x7f:												/* JNLE */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = IF;
		instruction->flags = 0;
		instruction->dstA.store = STORE_DIRECT;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		/* Means get from rest of instruction */
		relative = getbyte(base_address, offset + dis_instructions->bytes_used);
		/* extends byte to int64_t */
		instruction->dstA.index = relative;
		instruction->dstA.relocated = 0;
		dis_instructions->bytes_used+=1;
		instruction->dstA.value_size = 4;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = byte & 0xf; /* CONDITION */
		instruction->srcA.relocated = 0;
		instruction->srcA.value_size = 4;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0x80:												/* Grpl Eb,Ib */
		tmp = rmb(handle, dis_instructions, base_address, offset, 1, rex, &reg, &half);
		if (!tmp) {
			result = 0;
			break;
		}
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
		instruction->opcode = immed_table[reg];
		instruction->flags = 1;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		/* FIXME: This may sometimes be a word and not dword */
		instruction->srcA.index = getbyte(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		instruction->srcA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		dis_instructions->bytes_used += 1;
		instruction->srcA.value_size = 1;
		/* FIXME: !half bad */
		if (!half) {
    			instruction->dstA.indirect = IND_MEM;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.store = STORE_REG;
			if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
			    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
				instruction->dstA.indirect = IND_STACK; /* SP and BP use STACK memory and not DATA memory. */
				instruction->dstA.indirect_size = 8;
			}
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 1;
		}
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0x81:												/* Grpl Ev,Iv */
		tmp = rmb(handle, dis_instructions, base_address, offset, width, rex, &reg, &half);
		if (!tmp) {
			result = 0;
			break;
		}
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
		instruction->opcode = immed_table[reg];
		instruction->flags = 1;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		/* FIXME: This may sometimes be a word and not dword */
		instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		instruction->srcA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		dis_instructions->bytes_used += width;
		instruction->srcA.value_size = width;
		/* FIXME: !half bad */
		if (!half) {
    			instruction->dstA.indirect = IND_MEM;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.store = STORE_REG;
			if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
			    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
				instruction->dstA.indirect = IND_STACK; /* SP and BP use STACK memory and not DATA memory. */
				instruction->dstA.indirect_size = 8;
			}
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 4;
		}
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0x82:												/* Grpl Eb,Ib Mirror instruction */
		break;
	case 0x83:												/* Grpl Ev,Ix */
		tmp = rmb(handle, dis_instructions, base_address, offset, width, rex, &reg, &half);
		if (!tmp) {
			result = 0;
			break;
		}
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = immed_table[reg];
		instruction->flags = 1;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		relative = getbyte(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		long_signed = relative;
		instruction->srcA.index = long_signed;
		instruction->srcA.relocated = 0;
		dis_instructions->bytes_used += 1;
		instruction->srcA.value_size = width;
		/* FIXME: !half bad */
		if (!half) {
    			instruction->dstA.indirect = IND_MEM;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.store = STORE_REG;
			if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
			    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
				instruction->dstA.indirect = IND_STACK; /* SP and BP use STACK memory and not DATA memory. */
				instruction->dstA.indirect_size = 8;
			}
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = width;
		}
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0x84:												/* TEST Eb,Gb */
		result = dis_Ex_Gx(handle, TEST, rex, dis_instructions, base_address, offset, &reg, 1);
		break;
	case 0x85:												/* TEST Ev,Gv */
		result = dis_Ex_Gx(handle, TEST, rex, dis_instructions, base_address, offset, &reg, width);
		break;
	case 0x86:												/* XCHG Eb,Gb */
		break;
	case 0x87:												/* XCHG Ev,Gv */
		break;
	case 0x88:												/* MOV Eb,Gb */
		result = dis_Ex_Gx(handle, MOV, rex, dis_instructions, base_address, offset, &reg, 1);
		break;
	case 0x89:												/* MOV Ev,Gv */
		/* FIXME: Cannot handle 89 16 */
		/* FIXED: Cannot handle 4c 89 64 24 08 */
		/* FIXME: Cannot handle 89 05 NN NN NN NN */
		result = dis_Ex_Gx(handle, MOV, rex, dis_instructions, base_address, offset, &reg, width);
		break;
	case 0x8a:												/* MOV Gb,Eb */
		result = dis_Gx_Ex(handle, MOV, rex, dis_instructions, base_address, offset, &reg, 1);
		break;
	case 0x8b:
		/* MOV Gv,Ev */
		result = dis_Gx_Ex(handle, MOV, rex, dis_instructions, base_address, offset, &reg, width);
		break;
#if 0
		/* FIXME: Cannot handle 8b 15 00 00 00 00 */
		/* FIXME: Cannot handle 8b 45 fc */
		/* FIXME: Cannot handle 48 8b 05 00 00 00 00 */
		//result = dis_Gx_Ex(handle, MOV, dis_instructions, base_address, offset, &reg, 4);
		tmp = rmb(handle, dis_instructions, base_address, offset, width, rex, &reg, &half);
		if (!tmp) {
			result = 0;
			break;
		}
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = MOV;
		instruction->flags = 0;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = reg_table[reg].offset;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = width;
		if (!half) {
			printf("!half number=%d\n", dis_instructions->instruction_number);
			printf("inst[0].srcA.index = %"PRIx64"\n",
				dis_instructions->instruction[0].srcA.index);
			instruction->srcA.indirect = IND_MEM;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.store = STORE_REG;
			if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
			    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
				instruction->srcA.indirect = IND_STACK; /* SP and BP use STACK memory and not DATA memory. */
				instruction->srcA.indirect_size = 8;
			}
			instruction->srcA.index = REG_TMP1;
			instruction->srcA.value_size = width;
		}
		//if (!half) {
		//	instruction->srcA.store = STORE_REG;
		//	instruction->srcA.indirect = IND_DIRECT;
		//	instruction->srcA.index = REG_TMP1;
		//	instruction->srcA.value_size = 4;
		//}
		dis_instructions->instruction_number++;
		result = 1;
		break;
#endif
	case 0x8c:												/* Mov Ew,Sw */
		break;
	case 0x8d:												/* LEA Gv */
		tmp = rmb(handle, dis_instructions, base_address, offset, width, rex, &reg, &half);
		if (!tmp) {
			result = 0;
			break;
		}
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = MOV;
		instruction->flags = 0;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = reg_table[reg].offset;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = width;
		if (!half) {
			instruction->srcA.store = STORE_REG;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.index = REG_TMP1;
			instruction->srcA.value_size = 8;
		}
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0x8e:												/* MOV Sw,Ew */
		break;
	case 0x8f:												/* POP Ev */
		break;
	case 0x90:												/* NOP */
		result = 1;
		break;
	case 0x91:												/* XCHG CX,eAX */
		break;
	case 0x92:												/* XCHG DX,eAX */
		break;
	case 0x93:												/* XCHG BX,eAX */
		break;
	case 0x94:												/* XCHG SP,eAX */
		break;
	case 0x95:												/* XCHG BP,eAX */
		break;
	case 0x96:												/* XCHG SI,eAX */
		break;
	case 0x97:												/* XCHG DI,eAX */
		break;
	case 0x98:
		/* CBW: Signed extention. */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = SEX;
		instruction->flags = 0;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = REG_AX;
		instruction->srcA.value_size = width / 2;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_AX;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = width;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0x99:
		/* CWD: Signed extention. */
		break;
	case 0x9a:												/* CALL Ap */
	case 0x9b:												/* WAIT */
	case 0x9c:												/* PUSHF */
	case 0x9d:												/* POPF */
		break;
	case 0x9e:												/* SAHF */
		break;
	case 0x9f:												/* LAHF */
		break;
	case 0xa0:												/* MOV AL,Ob */
		break;
	case 0xa1:		/* MOV eAX,Ow */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = MOV;
		instruction->flags = 0;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_MEM;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		dis_instructions->bytes_used+=4;
		instruction->srcA.value_size = 4;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_AX;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 4;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xa2:												/* MOV Ob,AL */
		break;
	case 0xa3:		/* MOV Ow,eAX */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = MOV;
		instruction->flags = 0;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = REG_AX;
		instruction->srcA.value_size = 4;
		instruction->dstA.store = STORE_DIRECT;
		instruction->dstA.indirect = IND_MEM;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = getdword(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		instruction->dstA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			instruction->dstA.relocated = 1;
		}
		dis_instructions->bytes_used+=4;
		instruction->dstA.value_size = 4;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xa4:												/* MOVSB */
	case 0xa5:												/* MOVSW */
	case 0xa6:												/* CMPSB */
	case 0xa7:												/* CMPSW */
		break;
	case 0xa8:												/* TEST AL,Ib */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = TEST;
		instruction->flags = 0;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 1;
		instruction->srcA.index = getbyte(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		instruction->srcA.relocated = 0;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 1, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		dis_instructions->bytes_used+=1;
		instruction->srcA.value_size = 1;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 1;
		instruction->dstA.index = REG_AX;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xa9:												/* TEST eAX,Iv */
	case 0xaa:												/* STOSB */
	case 0xab:												/* STOSW */
	case 0xac:												/* LODSB */
	case 0xad:												/* LODSW */
	case 0xae:												/* SCASB */
	case 0xaf:												/* SCASW */
	case 0xb0:												/* MOV AL,Ib */
	case 0xb1:												/* MOV CL,Ib */
	case 0xb2:												/* MOV DL,Ib */
	case 0xb3:												/* MOV BL,Ib */
	case 0xb4:												/* MOV AH,Ib */
	case 0xb5:												/* MOV CH,Ib */
	case 0xb6:												/* MOV DH,Ib */
	case 0xb7:												/* MOV BH,Ib */
		break;
	case 0xb8:												/* MOV eAX,Iv */
	case 0xb9:												/* MOV CX,Iv */
	case 0xba:												/* MOV DX,Iv */
	case 0xbb:												/* MOV BX,Iv */
	case 0xbc:												/* MOV SP,Iv */
	case 0xbd:												/* MOV BP.Iv */
	case 0xbe:												/* MOV SI,Iv */
	case 0xbf:												/* MOV DI,Iv */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
		instruction->opcode = MOV;
		instruction->flags = 0;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = width;
		// Means get from rest of instruction
		instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used);
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			instruction->srcA.relocated = 1;
		}
		dis_instructions->bytes_used += 4;
		instruction->srcA.value_size = width;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = width;
		tmp = (byte & 0x7);
		if (rex & 0x1) {
			/* Register extention */
			/* Value 0x1 determined by disassemply observations */
			tmp = tmp | 0x8;
		}
		instruction->dstA.index = reg_table[tmp].offset;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = width;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xc0:												/* GRP2 Eb,Ib */
		tmp = rmb(handle, dis_instructions, base_address, offset, 1, rex, &reg, &half);
		if (!tmp) {
			result = 0;
			break;
		}
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = shift2_table[reg];
		instruction->flags = 1;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = getbyte(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		dis_instructions->bytes_used++;
		instruction->srcA.value_size = width;
		if (!half) {
    			instruction->dstA.indirect = IND_MEM;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.store = STORE_REG;
			if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
			    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
				instruction->dstA.indirect = IND_STACK; /* SP and BP use STACK memory and not DATA memory. */
				instruction->dstA.indirect_size = 8;
			}
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = width;
		}
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xc1:												/* GRP2 Ev,Ib */
		tmp = rmb(handle, dis_instructions, base_address, offset, width, rex, &reg, &half);
		if (!tmp) {
			result = 0;
			break;
		}
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = shift2_table[reg];
		instruction->flags = 1;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = getbyte(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		dis_instructions->bytes_used++;
		instruction->srcA.value_size = width;
		if (!half) {
    			instruction->dstA.indirect = IND_MEM;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.store = STORE_REG;
			if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
			    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
				instruction->dstA.indirect = IND_STACK; /* SP and BP use STACK memory and not DATA memory. */
				instruction->dstA.indirect_size = 8;
			}
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = width;
		}
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xc2:												/* RETN Iv */
		break;
	case 0xc3:												/* RETN */
                /* POP -> IP=[SP]; SP=SP+4; */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = MOV;
		instruction->flags = 0;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_TMP1;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 8;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_STACK;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = REG_SP;
		instruction->srcA.value_size = 8;
		dis_instructions->instruction_number++;

		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = ADD;
		instruction->flags = 0;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = 8;
		instruction->srcA.value_size = 8;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_SP;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 8;
		dis_instructions->instruction_number++;

		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = NOP;
		instruction->flags = 0;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_AX;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 8;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = REG_AX;
		instruction->srcA.value_size = 8;
		dis_instructions->instruction_number++;

		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = MOV;
		instruction->flags = 0;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_IP;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 8;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = REG_TMP1;
		instruction->srcA.value_size = 8;
		dis_instructions->instruction_number++;

		result = 1;
		break;
	case 0xc4:												/* LES */
	case 0xc5:												/* LDS */
	case 0xc6:												/* MOV Eb,Ib */
		result = dis_Ex_Ix(handle, MOV, rex, dis_instructions, base_address, offset, &reg, 1);
		break;
	case 0xc7:												/* MOV EW,Iv */
		/* JCD: Work in progress */
		result = dis_Ex_Ix(handle, MOV, rex, dis_instructions, base_address, offset, &reg, width);
		break;
	case 0xc8:												/* ENTER Iv,Ib */
		break;
	case 0xc9:												/* LEAVE */
		/* ESP = EBP; */
		/* POP EBP -> EBP=[SP]; SP=SP+4 (+2 for word); */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
		instruction->opcode = MOV;
		instruction->flags = 0;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_SP;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 8;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = REG_BP;
		instruction->srcA.value_size = 8;
		dis_instructions->instruction_number++;

		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
		instruction->opcode = MOV;
		instruction->flags = 0;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_BP;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 8;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_STACK;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = REG_SP;
		instruction->srcA.value_size = 8;
		dis_instructions->instruction_number++;

		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
		instruction->opcode = ADD;
		instruction->flags = 0;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = 8;
		instruction->srcA.value_size = 8;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_SP;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 8;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xca:												/* RETF Iv */
	case 0xcb:												/* RETF */
	case 0xcc:												/* INT3 */
	case 0xcd:												/* INT Ib */	
		break;
	case 0xce:												/* INTO */
		break;
	case 0xcf:												/* IRET */
	case 0xd0:												/* GRP2 Eb,1 */
	case 0xd1:												/* GRP2 Ev,1 */
		tmp = rmb(handle, dis_instructions, base_address, offset, width, rex, &reg, &half);
		if (!tmp) {
			result = 0;
			break;
		}
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = shift2_table[reg];
		instruction->flags = 1;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 1;
		instruction->srcA.index = 1;
		instruction->srcA.value_size = 1;
		if (!half) {
    			instruction->dstA.indirect = IND_MEM;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.store = STORE_REG;
			if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
			    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
				instruction->dstA.indirect = IND_STACK; /* SP and BP use STACK memory and not DATA memory. */
				instruction->dstA.indirect_size = 8;
			}
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = width;
		}
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xd2:												/* GRP2 Eb,CL */
		break;
	case 0xd3:												/* GRP2 Ev,CL */
		tmp = rmb(handle, dis_instructions, base_address, offset, width, rex, &reg, &half);
		if (!tmp) {
			result = 0;
			break;
		}
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = shift2_table[reg];
		instruction->flags = 1;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 1;
		instruction->srcA.index = REG_CX;
		instruction->srcA.value_size = 1;
		if (!half) {
    			instruction->dstA.indirect = IND_MEM;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.store = STORE_REG;
			if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
			    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
				instruction->dstA.indirect = IND_STACK; /* SP and BP use STACK memory and not DATA memory. */
				instruction->dstA.indirect_size = 8;
			}
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = width;
		}
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xd4:												/* AAM Ib */
	case 0xd5:												/* AAD Ib */
	case 0xd6:												/* SALC */
		break;
	case 0xd7:												/* XLAT */
		break;
	case 0xd8:												/* FPU ESC 0 */
	case 0xd9:												/* FPU ESC 1 */
	case 0xda:												/* FPU ESC 2 */
	case 0xdb:												/* FPU ESC 3 */
	case 0xdc:												/* FPU ESC 4 */
	case 0xdd:												/* FPU ESC 5 */
	case 0xde:												/* FPU ESC 6 */
	case 0xdf:												/* FPU ESC 7 */
		break;
	case 0xe0:												/* LOOPNZ */
		break;
	case 0xe1:												/* LOOPZ */
		break;
	case 0xe2:												/* LOOP */
		break;
	case 0xe3:												/* JCXZ */
	case 0xe4:												/* IN AL,Ib */
		break;
	case 0xe5:												/* IN eAX,Ib */
		break;
	case 0xe6:												/* OUT Ib,AL */
		break;
	case 0xe7:												/* OUT Ib,eAX */
		break;
	case 0xe8:												/* CALL Jw */
#if 0
                /* PUSH -> SP=SP-4 (-2 for word); [SP]=IP; */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = SUB;
		instruction->flags = 0;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.index = 4;
		instruction->srcA.value_size = 4;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.index = REG_SP;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 4;
		dis_instructions->instruction_number++;

		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = MOV;
		instruction->flags = 0;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.index = REG_IP;
		instruction->srcA.value_size = 4;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_STACK;
		instruction->dstA.index = REG_SP;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 4;
		dis_instructions->instruction_number++;
		/* Fall through to JMP */
#endif
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = CALL;
		instruction->flags = 0;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		// Means get from rest of instruction
		instruction->srcA.index = getdword(base_address, offset + dis_instructions->bytes_used);
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			printf("CALL RELOCATED 0x%04"PRIx64"\n", reloc_table_entry->value);
			instruction->srcA.relocated = 1;
			/* FIXME: Check it works for all related cases. E.g. jmp and call */

			instruction->srcA.index = reloc_table_entry->external_functions_index;
		}
		dis_instructions->bytes_used+=4;
		instruction->srcA.value_size = 4;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_AX;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 8;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xe9:												/* JMP Jw */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = JMP;
		instruction->flags = 0;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		// Means get from rest of instruction
		rel32 = getdword(base_address, offset + dis_instructions->bytes_used);
		rel64 = rel32;
		instruction->srcA.index = rel64;
		tmp = relocated_code(handle, base_address, offset + dis_instructions->bytes_used, 4, &reloc_table_entry);
		if (!tmp) {
			printf("RELOCATED 0x%04"PRIx64"\n", offset + dis_instructions->bytes_used);
			instruction->srcA.relocated = 1;
			/* FIXME: Check it works for all related cases. E.g. jmp and call */
			instruction->srcA.index = offset + dis_instructions->bytes_used;
		}
		dis_instructions->bytes_used+=4;
		instruction->srcA.value_size = 4;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_IP;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 4;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xea:												/* JMP Ap */
		break;
	case 0xeb:												/* JMP Jb */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = JMP;
		instruction->flags = 0;
		instruction->srcA.store = STORE_DIRECT;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		relative = getbyte(base_address, offset + dis_instructions->bytes_used); // Means get from rest of instruction
		instruction->srcA.index = relative;
		dis_instructions->bytes_used+=1;
		instruction->srcA.value_size = 4;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_IP;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 4;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xec:												/* IN AL,DX */
		break;
	case 0xed:												/* IN eAX,DX */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = IN;
		instruction->flags = 0;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_MEM;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = REG_DX;
		instruction->srcA.value_size = 4;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_DIRECT;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_AX;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 4;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xee:												/* OUT DX,AL */
		break;
	case 0xef:												/* OUT DX,eAX */
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
		instruction->opcode = OUT;
		instruction->flags = 0;
		instruction->srcA.store = STORE_REG;
		instruction->srcA.indirect = IND_DIRECT;
		instruction->srcA.indirect_size = 8;
		instruction->srcA.index = REG_AX;
		instruction->srcA.value_size = 4;
		instruction->dstA.store = STORE_REG;
		instruction->dstA.indirect = IND_MEM;
		instruction->dstA.indirect_size = 8;
		instruction->dstA.index = REG_DX;
		instruction->dstA.relocated = 0;
		instruction->dstA.value_size = 4;
		dis_instructions->instruction_number++;
		result = 1;
		break;
	case 0xf0:												/* LOCK */
		break;
	case 0xf2:												/* REPNZ */
		break;		
	case 0xf3:												/* REPZ */
		break;		
	case 0xf4:												/* HLT */
	case 0xf5:												/* CMC */
		break;
	case 0xf6:												/* GRP3 Eb ,Ib: */
		result = dis_Ex(handle, grp3_table, rex, dis_instructions, base_address, offset, 1);
		break;
	case 0xf7:												/* GRP3 Ev ,Iv: */
		result = dis_Ex(handle, grp3_table, rex, dis_instructions, base_address, offset, width);
		break;
	case 0xf8:												/* CLC */
		break;
	case 0xf9:												/* STC */
		break;
	case 0xfa:												/* CLI */
		break;
	case 0xfb:												/* STI */
		break;
	case 0xfc:												/* CLD */
		break;
	case 0xfd:												/* STD */
		break;
	case 0xfe:												/* GRP4 Eb */
		break;
	case 0xff:												/* GRP5 Ev */
		/* FIXED: Cannot handle: 41 ff 54 24 08 */
		half = base_address[offset + dis_instructions->bytes_used] & 0x38;
		if (half == 0x30) { /* Special for the PUSH case */
			instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
			instruction->opcode = SUB;  /* ESP = ESP - 4 */
			instruction->flags = 0; /* Do not effect flags */
			instruction->srcA.store = STORE_DIRECT;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.index = 4;
			instruction->srcA.value_size = 4;
			instruction->dstA.store = STORE_REG;
			instruction->dstA.indirect = IND_DIRECT;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.index = REG_SP;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 4;
			dis_instructions->instruction_number++;

		}
		tmp = rmb(handle, dis_instructions, base_address, offset, width, rex, &reg, &half);
		printf("Unfinished section 0xff\n");
		printf("half=0x%x, reg=0x%x\n",half, reg);
		if (!tmp) {
			result = 0;
			break;
		}
		instruction = &dis_instructions->instruction[dis_instructions->instruction_number];
		switch(reg) {
		case 0:
			instruction->opcode = ADD; /* INC by 1*/
			instruction->flags = 1;
			instruction->srcA.store = STORE_DIRECT;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.index = 1;
			instruction->srcA.value_size = 4;
			if (!half) {
				instruction->dstA.store = STORE_REG;
	
				instruction->dstA.indirect = IND_MEM;
				instruction->dstA.indirect_size = 8;
				if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
				    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
					/* SP and BP use STACK memory and not DATA memory. */
					instruction->dstA.indirect = IND_STACK;
					instruction->dstA.indirect_size = 8;
				}
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 4;
			}
			dis_instructions->instruction_number++;
			result = 1;
			break;
		case 1:
			instruction->opcode = SUB; /* DEC by 1 */
			instruction->flags = 1;
			instruction->srcA.store = STORE_DIRECT;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.index = 1;
			instruction->srcA.value_size = 4;
			instruction->dstA.store = STORE_REG;
			instruction->dstA.indirect = IND_MEM;
			instruction->dstA.indirect_size = 8;
			if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
			    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
				/* SP and BP use STACK memory and not DATA memory. */
				instruction->dstA.indirect = IND_STACK;
				instruction->dstA.indirect_size = 8;
			}
			instruction->dstA.index = REG_TMP1;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 4;
			dis_instructions->instruction_number++;
			result = 1;
			break;
		case 2:
			instruction->opcode = CALL; /* CALL rm. E.g. ff 53 08 callq  *0x8(%rbx)    */
			instruction->flags = 0;
			instruction->srcA.store = STORE_DIRECT;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.index = 1;
			instruction->srcA.value_size = 4;
			if (!half) {
				instruction->dstA.store = STORE_REG;
	
				instruction->dstA.indirect = IND_MEM;
				instruction->dstA.indirect_size = 8;
				if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
				    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
					/* SP and BP use STACK memory and not DATA memory. */
					instruction->dstA.indirect = IND_STACK;
					instruction->dstA.indirect_size = 8;
				}
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 4;
			}
			dis_instructions->instruction_number++;
			result = 1;
			break;
		case 4:
			printf("JMP Inst: 0xFF\n");
			/* For now, get the EXE to assume the function has ended. */
			/* Do this by setting EIP = 0 */
//			instruction = &dis_instructions->instruction[dis_instructions->instruction_number];	
			instruction->opcode = MOV;
			instruction->flags = 0;
			instruction->srcA.store = STORE_DIRECT;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.index = 0;
			instruction->srcA.value_size = 8;
			instruction->dstA.store = STORE_REG;
			instruction->dstA.indirect = IND_DIRECT;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.index = REG_IP;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 8;
			dis_instructions->instruction_number++;
#if 0


			instruction->opcode = JMP; /* JMP rm. E.g. ff 24 c5 00 00 00 00 jmpq   *0x0(,%rax,8)   */
			instruction->flags = 0;
			instruction->srcA.store = STORE_DIRECT;
			instruction->srcA.indirect = IND_DIRECT;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.index = 1;
			instruction->srcA.value_size = 4;
			if (!half) {
				instruction->dstA.store = STORE_REG;
	
				instruction->dstA.indirect = IND_MEM;
				instruction->dstA.indirect_size = 8;
				if ((dis_instructions->instruction[0].srcA.index >= REG_SP) && 
				    (dis_instructions->instruction[0].srcA.index <= REG_BP) ) {
					/* SP and BP use STACK memory and not DATA memory. */
					instruction->dstA.indirect = IND_STACK;
					instruction->dstA.indirect_size = 8;
				}
				instruction->dstA.index = REG_TMP1;
				instruction->dstA.relocated = 0;
				instruction->dstA.value_size = 4;
			}
			dis_instructions->instruction_number++;
#endif
			result = 1;
			break;
		case 6: /* FIXME: not correct yet */
			/* the pushl -4[%ecx] case */
			/* Need to work out if -4[%ecx] is
			 * tmp=%ecx;
			 * tmp-=4
			 * final=[tmp]
			 * or is instead
			 * tmp=[%ecx];
			 * tmp-=4;
			 * final=tmp;
			 */
			instruction->opcode = MOV; /* PUSH is a ESP sub followed by a MOV */
			instruction->flags = 0;
			instruction->srcA.store = STORE_REG;
			instruction->srcA.indirect = IND_MEM;
			instruction->srcA.indirect_size = 8;
			instruction->srcA.index = REG_TMP1;
			instruction->srcA.value_size = 4;
			instruction->dstA.store = STORE_REG;
			/* due to special PUSH case, with added
			 * instruction before rmb
			 */
			/* SP and BP use STACK memory and not DATA memory. */
			instruction->dstA.indirect = IND_STACK;
			instruction->dstA.indirect_size = 8;
			instruction->dstA.index = REG_SP;
			instruction->dstA.relocated = 0;
			instruction->dstA.value_size = 4;
			dis_instructions->instruction_number++;
			result = 1;
			break;
		default:
			dis_instructions->instruction_number=0; /* Tag unimplemented dis_instructions. */
			break;
		}
	}

	return result;
}
