/*
 * Implementation of own version of BASE85 binary data encoding
 * adapted for the usage inside Ruby source code (i.e. without
 * such symbols as \ " # { } '
 *
 * Format of the output stream:
 * 1) First byte: number of bytes in the last chunk: from 0 to 3
 *    (0 means that the last chunk contains 4 bytes, i.e. everything
 *     is aligned). See val_to_char array for the used alphabet
 * 2) big-endian 5-byte numbers (base 85)
 * 3) empty string: arbitrary two bytes
 *
 * (C) 2015 Alexey Voskov
 * License: 2-clause BSD
 */
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <ruby.h>
#include <ruby/version.h>

#define BASE85R_STR_WIDTH 14 // Number of 5-byte groups in the string (12 for 60-byte string)

#define D0_VAL 1 // 85**0
#define D1_VAL 85 // 85**1
#define D2_VAL 7225 // 85**2
#define D3_VAL 614125 // 85**3
#define D4_VAL 52200625 // 85**4

static int di_val[5] = {D4_VAL, D3_VAL, D2_VAL, D1_VAL, D0_VAL};


/* Modified BASE85 digits */
static const char val_to_char[86] = // ASCIIZ string
"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
"abcdefghijklmnopqrstuvwxyz"
"0123456789"
"!$%&()*-./"
":;<=>?@[]^"
",_|";

static int char_to_val[128];


/* Initializes internal tables that are required
   for recoding */
void base85r_init_tables()
{
	int i;
	for (i = 0; i < 128; i++)
		char_to_val[i] = -1;


	for (i = 0; i < 85; i++)
	{
		if (char_to_val[(int) val_to_char[i]] != -1)
			rb_raise(rb_eArgError, "Internal error");
		char_to_val[(int) val_to_char[i]] = i;
	}

	for (i = 0; i < 85; i++)
		if (char_to_val[(int) val_to_char[i]] != i)
			rb_raise(rb_eArgError, "Internal error");
}


/* Calculate length of buffer for base85 encoding */
static int base85_encode_buf_len(int len)
{
	// Calculate aligned (32-bit alignment) size of the buffer
	int buf_len = ((len >> 2) << 2);
	if (len % 4) buf_len += 4;
	// Calculate size of the output buffer
	buf_len = (buf_len * 5) / 4;
	buf_len = (buf_len * 105) / 100;
	buf_len += 32;
	// Return buffer size
	return buf_len;
}

/* Encode string to modified BASE85 ASCII.
   Call base85_init_tables before using of this function */
VALUE base85r_encode(VALUE input)
{
	VALUE output;
	int inp_len, out_len, out_buf_len;
	int pos, outpos;
	unsigned int val;
	unsigned char *outptr, *inptr;
	int i;
	// Check input data type and allocate string
	if (TYPE(input) != T_STRING)
		rb_raise(rb_eArgError, "base85r_encode: input must be a string");
	inp_len = RSTRING_LEN(input);
	out_buf_len = base85_encode_buf_len(inp_len);
	output = rb_str_new(NULL, out_buf_len);
	// Begin conversion
	outpos = 0;
	outptr = (unsigned char *) RSTRING_PTR(output);
	inptr = (unsigned char *) RSTRING_PTR(input);
	outptr[outpos++] = 32;
	outptr[outpos++] = val_to_char[inp_len % 4];
	out_len = 2;
	for (pos = 0; pos < inp_len; )
	{
		// Get four bytes
		val = 0;
		for (i = 24; i >= 0; i -= 8)
			if (pos < inp_len) val |= inptr[pos++] << i;
		// And transform them to five bytes
		for (i = 0; i < 5; i++)
		{
			int digit = (val / di_val[i]) % 85;
			char sym = val_to_char[digit];
			outptr[outpos++] = sym;
		}
		out_len += 5;
		// Newline addition
		if (pos % (4 * BASE85R_STR_WIDTH) == 0)
		{
			out_len += 2;
			outptr[outpos++] = 10;
			outptr[outpos++]= 32;
		}
	}
	// Check the state of memory
	if (outpos >= out_buf_len)
		rb_raise(rb_eArgError, "base85r_encode: internal memory error");
	// Truncate the empty "tail" of the buffer and return the string 
	return rb_str_resize(output, out_len);
}


/* Decode string in modified BASE85 ASCII format.
   Call base85_init_tables before using of this function */
VALUE base85r_decode(VALUE input)
{
	int inp_len, out_len, pos, shift;
	unsigned int val = 0;
	VALUE output;
	unsigned char *inptr, *outptr;
	int tail_len, i;
	// Check input data type and allocate string
	if (TYPE(input) != T_STRING)
		rb_raise(rb_eArgError, "base85r_decode: input must be a string");
	inp_len = RSTRING_LEN(input);
	if (inp_len < 6 && inp_len != 2)
	{	// String with 1 or more symbols
		rb_raise(rb_eArgError, "base85r_decode: input string is too short");
	}
	output = rb_str_new(NULL, inp_len);
	// Begin conversion
	inptr = (unsigned char *) RSTRING_PTR(input);
	outptr = (unsigned char *) RSTRING_PTR(output);
	out_len = 0;
	tail_len = -1;
	shift = 0;
	val = 0;
	for (pos = 0; pos < inp_len; pos++)
	{
		int digit = char_to_val[(int) inptr[pos]];
		if (digit != -1)
		{
			if (tail_len == -1)
			{
				tail_len = digit;
				if (tail_len > 4)
					rb_raise(rb_eArgError, "base85r_decode: input string is corrupted");
				continue;
			}
			val +=  digit * di_val[shift++]; 
			if (shift == 5)
			{
				for (i = 24; i >= 0; i -= 8)
					*outptr++ = (val >> i) & 0xFF;
				shift = 0; val = 0;
				out_len += 4;
			}
		}
	}
	// Check if the byte sequence was valid
	if (shift != 0)
		rb_raise(rb_eArgError, "base85r_decode: input string is corrupted");
	// Take into account unaligned "tail"
	if (tail_len != 0)
	{
		out_len -= (4 - tail_len);
	}
	return rb_str_resize(output, out_len);
}
