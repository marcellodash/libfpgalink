/* 
 * Copyright (C) 2011 Chris McClelland
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *  
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdlib.h>
#include <makestuff.h>
#include <liberror.h>
#include <libnero.h>
#include "firmware/defs.h"
#include "xsvf.h"
#include "csvfplay.h"

// -------------------------------------------------------------------------------------------------
// Declaration of private types & functions
// -------------------------------------------------------------------------------------------------

typedef struct {
	const uint8 *data;
	uint16 count;
	bool isReadingChunk;
} Context;

static uint8 getRawByte(Context *cp);
static uint16 readLength(Context *cp);
static uint8 getNextByte(Context *cp);
static void dumpSimple(const unsigned char *input, unsigned int length, char *p);
static uint16 getWord(Context *cp);
static uint32 getLong(Context *cp);

// -------------------------------------------------------------------------------------------------
// Public functions
// -------------------------------------------------------------------------------------------------

// Play the uncompressed CSVF stream into the JTAG port.
//
int csvfPlay(const uint8 *csvfData, struct NeroHandle *nero, const char **error) {
	int returnCode;
	NeroStatus nStatus;
	uint8 thisByte;
	uint32 numBytes;
	uint8 *ptr;
	uint8 i;
	uint32 xsdrSize = 0;
	uint16 xruntest = 0;
	uint8 tdoMask[128];
	uint8 tdiData[128];
	uint8 tdoData[128];
	uint8 tdoExpected[128];
	uint8 *tdiAll;
	Context cp;

	nStatus = neroClockFSM(nero, 0x0000001F, 6, error);  // Reset TAP, goto Run-Test/Idle
	CHECK_STATUS(nStatus, "csvfPlay()", nStatus);

	cp.data = csvfData;
	thisByte = getRawByte(&cp);
	if ( thisByte ) {
		// Header byte may be used for something later. For now, ensure it's zero.
		errRender(error, "csvfPlay(): Bad CSVF header!");
		FAIL(CPLAY_HEADER_ERR);
	}
	cp.count = readLength(&cp);
	cp.isReadingChunk = true;

	thisByte = getNextByte(&cp);
	while ( thisByte != XCOMPLETE ) {
		switch ( thisByte ) {
		case XTDOMASK:
			numBytes = bitsToBytes(xsdrSize);
			ptr = tdoMask;
			while ( numBytes-- ) {
				*ptr++ = getNextByte(&cp);
			}
			break;

		case XRUNTEST:
			getNextByte(&cp);  // Ignore the MSW (realistically will it ever be nonzero?)
			getNextByte(&cp);
			xruntest = getWord(&cp);
			break;

		case XSIR:
			nStatus = neroClockFSM(nero, 0x00000003, 4, error);  // -> Shift-IR
			CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			thisByte = getNextByte(&cp);
			numBytes = bitsToBytes(thisByte);
			ptr = tdiData;
			while ( numBytes-- ) {
				*ptr++ = getNextByte(&cp);
			}
			nStatus = neroShift(nero, thisByte, tdiData, NULL, true, error);  // -> Exit1-DR
			CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			nStatus = neroClockFSM(nero, 0x00000001, 2, error);  // -> Run-Test/Idle
			CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			if ( xruntest ) {
				nStatus = neroClocks(nero, xruntest, error);
				CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			}
			break;

		case XSDRSIZE:
			xsdrSize = getLong(&cp);
			break;

		case XSDRTDO:
			nStatus = neroClockFSM(nero, 0x00000001, 3, error);  // -> Shift-DR
			CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			numBytes = bitsToBytes(xsdrSize);
			ptr = tdoExpected;
			while ( numBytes-- ) {
				*ptr++ = getNextByte(&cp);
			}
			numBytes = bitsToBytes(xsdrSize);
			ptr = tdiData;
			while ( numBytes-- ) {
				*ptr++ = getNextByte(&cp);
			}
			nStatus = neroShift(nero, xsdrSize, tdiData, tdoData, true, error);  // -> Exit1-DR
			CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			numBytes = bitsToBytes(xsdrSize);
			for ( i = 0; i < numBytes; i++ ) {
				if ( (tdoData[i] & tdoMask[i]) != (tdoExpected[i] & tdoMask[i]) ) {
					char data[CSVF_BUF_SIZE*2+1];
					char mask[CSVF_BUF_SIZE*2+1];
					char expected[CSVF_BUF_SIZE*2+1];
					dumpSimple(tdoData, numBytes, data);
					dumpSimple(tdoMask, numBytes, mask);
					dumpSimple(tdoExpected, numBytes, expected);
					errRender(
						error,
						"csvfPlay(): XSDRTDO failed:\n  Got: %s\n  Mask: %s\n  Expecting: %s",
						data, mask, expected);
					FAIL(CPLAY_COMPARE_ERR);
				}
			}
			nStatus = neroClockFSM(nero, 0x00000001, 2, error);  // -> Run-Test/Idle
			CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			if ( xruntest ) {
				nStatus = neroClocks(nero, xruntest, error);
				CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			}
			break;

		case XSDRB:
			nStatus = neroClockFSM(nero, 0x00000001, 3, error);  // -> Shift-DR
			CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			numBytes = bitsToBytes(xsdrSize);
			tdiAll = malloc(numBytes);
			ptr = tdiAll;
			while ( numBytes-- ) {
				*ptr++ = getNextByte(&cp);
			}
			nStatus = neroShift(nero, xsdrSize, tdiAll, NULL, false, error);  // -> Shift-DR
			free(tdiAll);
			CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			break;

		case XSDRE:
			numBytes = bitsToBytes(xsdrSize);
			ptr = tdiData;
			while ( numBytes-- ) {
				*ptr++ = getNextByte(&cp);
			}
			nStatus = neroShift(nero, xsdrSize, tdiData, NULL, true, error);  // -> Exit1-DR
			CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			nStatus = neroClockFSM(nero, 0x00000001, 2, error);  // -> Run-Test/Idle
			CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			if ( xruntest ) {
				nStatus = neroClocks(nero, xruntest, error);
				CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			}
			break;

		case XSTATE:
			// The XSTATE commands appear to be pretty superfluous. For now we can get away with
			// always switching to Run-Test/Idle when we get an XSTATE
			getNextByte(&cp);
			nStatus = neroClockFSM(nero, 0x0000001F, 6, error);  // -> Run-Test/Idle
			CHECK_STATUS(nStatus, "csvfPlay()", nStatus);
			break;

		default:
			errRender(error, "csvfPlay(): Unsupported command 0x%02X", thisByte);
			FAIL(CPLAY_UNKNOWN_CMD_ERR);
		}
		thisByte = getNextByte(&cp);
	}
	returnCode = NERO_SUCCESS;
cleanup:
	return returnCode;
}

// -------------------------------------------------------------------------------------------------
// Implementation of private functions
// -------------------------------------------------------------------------------------------------

// Read the next raw byte from the input stream
//
static uint8 getRawByte(Context *cp) {
	return *cp->data++;
}

// Read the length (of the chunk or the zero run). A short block (<256 bytes) length is encoded in a
// single byte. If that single byte is zero, we know it's a long block (256-65535 bytes), so read in
// the next two bytes as a big-endian uint16.
//
static uint16 readLength(Context *cp) {
	uint16 len = getRawByte(cp);
	if ( !len ) {
		len = getRawByte(cp);
		len <<= 8;
		len |= getRawByte(cp);
	}
	return len;
}

// Get the next byte from the uncompressed stream. Uses m_count & m_isReadingChunk to keep state.
//
static uint8 getNextByte(Context *cp) {
	if ( cp->isReadingChunk ) {
		// We're in the middle of reading a chunk.
		if ( cp->count ) {
			// There are still some bytes to copy verbatim into the uncompressed stream.
			cp->count--;
			return getRawByte(cp);
		} else {
			// We're at the end of this chunk; there will now be some zeros to insert into the
			// uncompressed stream.
			cp->count = readLength(cp);
			cp->isReadingChunk = false;
			return getNextByte(cp);
		}
	} else {
		// We're in the middle of a run of zeros.
		if ( cp->count ) {
			// There are still some zero bytes to write to the uncompressed stream.
			cp->count--;
			return 0x00;
		} else {
			// We're at the end of this run of zeros; there will now be a chunk of data to be copied
			// verbatim over to the uncompressed stream.
			cp->count = readLength(cp);
			cp->isReadingChunk = true;
			return getNextByte(cp);
		}
	}
}

static const char *const nibbles = "0123456789ABCDEF";

// Dump some hex bytes to a buffer.
//
static void dumpSimple(const unsigned char *input, unsigned int length, char *p) {
	uint8 upperNibble, lowerNibble;
	while ( length ) {
		upperNibble = lowerNibble = *input++;
		upperNibble >>= 4;
		lowerNibble &= 15;
		*p++ = nibbles[upperNibble];
		*p++ = nibbles[lowerNibble];
		--length;
	}
	*p = '\0';
}

// Get big-endian uint16 from the stream
//
static uint16 getWord(Context *cp) {
	uint16 value;
	value = getNextByte(cp);
	value <<= 8;
	value |= getNextByte(cp);
	return value;
}

// Get big-endian uint32 from the stream
//
static uint32 getLong(Context *cp) {
	uint32 value;
	value = getNextByte(cp);
	value <<= 8;
	value |= getNextByte(cp);
	value <<= 8;
	value |= getNextByte(cp);
	value <<= 8;
	value |= getNextByte(cp);
	return value;
}
