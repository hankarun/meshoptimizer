#include "../src/meshoptimizer.h"

#include <vector>

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifndef TRACE
#define TRACE 0
#endif

const unsigned char kVertexHeader = 0xa0;

static int gEncodeVertexVersion = 1;

const size_t kVertexBlockSizeBytes = 8192;
const size_t kVertexBlockMaxSize = 256;
const size_t kByteGroupSize = 16;
const size_t kByteGroupDecodeLimit = 24;
const size_t kTailMaxSize = 32;

static size_t getVertexBlockSize(size_t vertex_size)
{
	// make sure the entire block fits into the scratch buffer
	size_t result = kVertexBlockSizeBytes / vertex_size;

	// align to byte group size; we encode each byte as a byte group
	// if vertex block is misaligned, it results in wasted bytes, so just truncate the block size
	result &= ~(kByteGroupSize - 1);

	return (result < kVertexBlockMaxSize) ? result : kVertexBlockMaxSize;
}

inline unsigned char zigzag8(unsigned char v)
{
	return ((signed char)(v) >> 7) ^ (v << 1);
}

#if TRACE
struct Stats
{
	size_t size;
	size_t header;  // bytes for header
	size_t bitg[4]; // bytes for bit groups
	size_t bitc[8]; // bit consistency: how many bits are shared between all bytes in a group
};

static Stats* bytestats = NULL;
static Stats vertexstats[256];
#endif

static bool encodeBytesGroupZero(const unsigned char* buffer)
{
	for (size_t i = 0; i < kByteGroupSize; ++i)
		if (buffer[i])
			return false;

	return true;
}

static unsigned char* encodeBytesGroupTry(unsigned char* data, const unsigned char* buffer, int bits)
{
	assert(bits >= 0 && bits <= 8);

	if (bits == 0)
		return encodeBytesGroupZero(buffer) ? data : NULL;

	if (bits == 8)
	{
		memcpy(data, buffer, kByteGroupSize);
		return data + kByteGroupSize;
	}

	// fixed portion: bits bits for each value
	// variable portion: full byte for each out-of-range value (using 1...1 as sentinel)
	unsigned char sentinel = (1 << bits) - 1;

	unsigned int accum = 0;
	unsigned int accum_bits = 0;

	for (size_t i = 0; i < kByteGroupSize; ++i)
	{
		unsigned char enc = (buffer[i] >= sentinel) ? sentinel : buffer[i];

		accum <<= bits;
		accum |= enc;
		accum_bits += bits;

		if (accum_bits >= 8)
		{
			accum_bits -= 8;
			*data++ = accum >> accum_bits;
		}
	}

	assert(accum_bits == 0);

	for (size_t i = 0; i < kByteGroupSize; ++i)
	{
		if (buffer[i] >= sentinel)
		{
			*data++ = buffer[i];
		}
	}

	return data;
}

static unsigned char* encodeBytes(unsigned char* data, unsigned char* data_end, const unsigned char* buffer, size_t buffer_size,
    int bits0, int bits1, int bits2, int bits3)
{
	assert(buffer_size % kByteGroupSize == 0);

	unsigned char* header = data;

	// round number of groups to 4 to get number of header bytes
	size_t header_size = (buffer_size / kByteGroupSize + 3) / 4;

	if (size_t(data_end - data) < header_size)
		return NULL;

	data += header_size;

	memset(header, 0, header_size);

	for (size_t i = 0; i < buffer_size; i += kByteGroupSize)
	{
		if (size_t(data_end - data) < kByteGroupDecodeLimit)
			return NULL;

		int best_bits = -1;
		size_t best_size = SIZE_MAX;

		for (int bits = 0; bits <= 8; bits++)
		{
			if (bits != bits0 && bits != bits1 && bits != bits2 && bits != bits3)
				continue;

			unsigned char* next = encodeBytesGroupTry(data, buffer + i, bits);
			if (!next)
				continue;

			if (size_t(next - data) < best_size || (bits == 8 && size_t(next - data) == best_size))
			{
				best_bits = bits;
				best_size = next - data;
			}
		}

		int bitsenc = (best_bits == bits0) ? 0 : (best_bits == bits1 ? 1 : (best_bits == bits2 ? 2 : 3));

		size_t header_offset = i / kByteGroupSize;

		header[header_offset / 4] |= bitsenc << ((header_offset % 4) * 2);

		unsigned char* next = encodeBytesGroupTry(data, buffer + i, best_bits);

		assert(next && data + best_size == next);
		data = next;

#if TRACE
		bytestats->bitg[bitsenc] += best_size;
#endif
	}

#if TRACE
	bytestats->header += header_size;
#endif

	return data;
}

static unsigned char* encodeVertexBlock(unsigned char* data, unsigned char* data_end, const unsigned char* vertex_data, size_t vertex_count, size_t vertex_size, unsigned char last_vertex[256])
{
	assert(vertex_count > 0 && vertex_count <= kVertexBlockMaxSize);

	unsigned char buffer[kVertexBlockMaxSize];
	assert(sizeof(buffer) % kByteGroupSize == 0);

	// we sometimes encode elements we didn't fill when rounding to kByteGroupSize
	memset(buffer, 0, sizeof(buffer));

	unsigned char* header = data;

	// 2-bit selector per byte per block
	for (size_t k = 0; k < vertex_size; k += 4)
		*data++ = 0;

	for (size_t k = 0; k < vertex_size; ++k)
	{
		size_t vertex_offset = k;

		unsigned char p = last_vertex[k];

		for (size_t i = 0; i < vertex_count; ++i)
		{
			buffer[i] = zigzag8(vertex_data[vertex_offset] - p);

			p = vertex_data[vertex_offset];

			vertex_offset += vertex_size;
		}

#if TRACE
		const unsigned char* olddata = data;
		bytestats = &vertexstats[k];

		for (size_t ig = 0; ig < vertex_count; ig += kByteGroupSize)
		{
			unsigned char last = (ig == 0) ? last_vertex[k] : vertex_data[vertex_size * (ig - 1) + k];
			unsigned char delta = 0xff;

			for (size_t i = ig; i < ig + kByteGroupSize && i < vertex_count; ++i)
				delta &= ~(vertex_data[vertex_size * i + k] ^ last);

			for (int j = 0; j < 8; ++j)
				bytestats->bitc[j] += (vertex_count - ig < kByteGroupSize ? vertex_count - ig : kByteGroupSize) * ((delta >> j) & 1);
		}
#endif

		size_t vertex_count_aligned = (vertex_count + kByteGroupSize - 1) & ~(kByteGroupSize - 1);

		const int encs[4][4] =
		    {
		        {0, 2, 4, 8},
		        {0, 1, 2, 4},
		        {0, 1, 2, 8},
		        {0, 1, 6, 8},
		    };

		int best_enc = -1;
		size_t best_size = SIZE_MAX;

		for (int enc = 0; enc < 4; ++enc)
		{
			unsigned char* encp = encodeBytes(data, data_end, buffer, vertex_count_aligned, encs[enc][0], encs[enc][1], encs[enc][2], encs[enc][3]);
			assert(encp);

			if (size_t(encp - data) < best_size)
			{
				best_enc = enc;
				best_size = size_t(encp - data);
			}
		}

		header[k / 4] |= best_enc << ((k % 4) * 2);

		data = encodeBytes(data, data_end, buffer, vertex_count_aligned, encs[best_enc][0], encs[best_enc][1], encs[best_enc][2], encs[best_enc][3]);
		if (!data)
			return NULL;

#if TRACE
		bytestats = NULL;
		vertexstats[k].size += data - olddata;
#endif
	}

	memcpy(last_vertex, &vertex_data[vertex_size * (vertex_count - 1)], vertex_size);

	return data;
}

size_t encodeV1(unsigned char* buffer, size_t buffer_size, const void* vertices, size_t vertex_count, size_t vertex_size)
{
	assert(vertex_size > 0 && vertex_size <= 256);
	assert(vertex_size % 4 == 0);

#if TRACE
	memset(vertexstats, 0, sizeof(vertexstats));
#endif

	const unsigned char* vertex_data = static_cast<const unsigned char*>(vertices);

	unsigned char* data = buffer;
	unsigned char* data_end = buffer + buffer_size;

	if (size_t(data_end - data) < 1 + vertex_size)
		return 0;

	int version = gEncodeVertexVersion;

	*data++ = (unsigned char)(kVertexHeader | version);

	unsigned char first_vertex[256] = {};
	if (vertex_count > 0)
		memcpy(first_vertex, vertex_data, vertex_size);

	unsigned char last_vertex[256] = {};
	memcpy(last_vertex, first_vertex, vertex_size);

	size_t vertex_block_size = getVertexBlockSize(vertex_size);

	size_t vertex_offset = 0;

	while (vertex_offset < vertex_count)
	{
		size_t block_size = (vertex_offset + vertex_block_size < vertex_count) ? vertex_block_size : vertex_count - vertex_offset;

		data = encodeVertexBlock(data, data_end, vertex_data + vertex_offset * vertex_size, block_size, vertex_size, last_vertex);
		if (!data)
			return 0;

		vertex_offset += block_size;
	}

	size_t tail_size = vertex_size < kTailMaxSize ? kTailMaxSize : vertex_size;

	if (size_t(data_end - data) < tail_size)
		return 0;

	// write first vertex to the end of the stream and pad it to 32 bytes; this is important to simplify bounds checks in decoder
	if (vertex_size < kTailMaxSize)
	{
		memset(data, 0, kTailMaxSize - vertex_size);
		data += kTailMaxSize - vertex_size;
	}

	memcpy(data, first_vertex, vertex_size);
	data += vertex_size;

	assert(data >= buffer + tail_size);
	assert(data <= buffer + buffer_size);

#if TRACE
	size_t total_size = data - buffer;

	for (size_t k = 0; k < vertex_size; ++k)
	{
		const Stats& vsk = vertexstats[k];

		printf("%2d: %7d bytes [%4.1f%%] %.1f bpv", int(k), int(vsk.size), double(vsk.size) / double(total_size) * 100, double(vsk.size) / double(vertex_count) * 8);

		size_t total_k = vsk.header + vsk.bitg[0] + vsk.bitg[1] + vsk.bitg[2] + vsk.bitg[3];

		printf(" |\thdr [%5.1f%%] bitg 1-3 [%4.1f%% %4.1f%% %4.1f%%]",
		    double(vsk.header) / double(total_k) * 100, double(vsk.bitg[1]) / double(total_k) * 100,
		    double(vsk.bitg[2]) / double(total_k) * 100, double(vsk.bitg[3]) / double(total_k) * 100);

		printf(" |\tbitc [%3.0f%% %3.0f%% %3.0f%% %3.0f%% %3.0f%% %3.0f%% %3.0f%% %3.0f%%]",
		    double(vsk.bitc[0]) / double(vertex_count) * 100, double(vsk.bitc[1]) / double(vertex_count) * 100,
		    double(vsk.bitc[2]) / double(vertex_count) * 100, double(vsk.bitc[3]) / double(vertex_count) * 100,
		    double(vsk.bitc[4]) / double(vertex_count) * 100, double(vsk.bitc[5]) / double(vertex_count) * 100,
		    double(vsk.bitc[6]) / double(vertex_count) * 100, double(vsk.bitc[7]) / double(vertex_count) * 100);
		printf("\n");
	}
#endif

	return data - buffer;
}

template <typename G>
size_t encodeNDZ(unsigned char* buffer, size_t buffer_size, const void* vertices, size_t vertex_count, size_t vertex_size)
{
	unsigned char* pos = buffer;
	const size_t GS = sizeof(G);

	for (size_t k = 0; k < vertex_size; k += GS)
	{
		G last = 0;

		for (size_t i = 0; i < vertex_count; i += GS * 8)
		{
			G deltas[GS * 8] = {};

			for (size_t j = 0; j < GS * 8 && i + j < vertex_count; ++j)
			{
				G value = *(G*)((char*)vertices + (i + j) * vertex_size + k);

				// hurts unless values clump around 0
				// value = (value << 1) | (value >> 31);

				deltas[j] = value - last;

				G sign = 1u << (GS * 8 - 1);
				deltas[j] ^= (deltas[j] & sign) ? sign - 1 : 0;

				last = value;
			}

			G transposed[GS * 8] = {};
			for (size_t jr = 0; jr < GS * 8; ++jr)
				for (size_t jc = 0; jc < GS * 8; ++jc)
					if (deltas[jc] & (1u << jr))
						transposed[jr] |= 1u << jc;

			// effectively becomes raw deltas (for testing)
			// memcpy(transposed, deltas, sizeof(deltas));

			G mask = 0;
			for (size_t j = 0; j < GS * 8; ++j)
				if (transposed[j])
					mask |= 1u << j;

			*(G*)pos = mask;
			pos += GS;

			for (size_t j = 0; j < GS * 8; ++j)
				if (transposed[j])
				{
					*(G*)pos = transposed[j];
					pos += GS;
				}
		}
	}

	assert(pos <= buffer + buffer_size);
	return pos - buffer;
}

int main(int argc, char** argv)
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif

	if (argc < 2 || argc > 3 || atoi(argv[1]) <= 0)
	{
		fprintf(stderr, "Usage: %s <stride> [<count>]\n", argv[0]);
		return 1;
	}

	size_t stride = atoi(argv[1]);

	std::vector<unsigned char> input;
	unsigned char buffer[4096];
	size_t bytes_read;

	while ((bytes_read = fread(buffer, 1, sizeof(buffer), stdin)) > 0)
		input.insert(input.end(), buffer, buffer + bytes_read);

	int vec3 = getenv("VEC3") ? atoi(getenv("VEC3")) : 0;
	int norm = getenv("NORM") ? atoi(getenv("NORM")) : 0;

	if (argc == 2 && vec3)
	{
		size_t vertex_count = input.size() / stride;
		std::vector<unsigned char> inputpx;
		for (size_t i = 0; i < vertex_count; ++i)
			for (int k = 0; k < 12; ++k)
				inputpx.push_back(input[i * stride + k]);

		stride = 12;
		input.swap(inputpx);
	}
	else if (argc == 2 && norm)
	{
		size_t vertex_count = input.size() / stride;
		std::vector<unsigned char> inputpx;
		for (size_t i = 0; i < vertex_count; ++i)
			for (int k = 0; k < 4; ++k)
				inputpx.push_back(input[i * stride + (stride - 4) + k]);

		stride = 4;
		input.swap(inputpx);
	}

	if (argc == 3)
	{
		// if count is specified, we assume input is meshopt-encoded and decode it first
		size_t count = atoi(argv[2]);

		std::vector<unsigned char> decoded(count * stride);
		int res = meshopt_decodeVertexBuffer(&decoded[0], count, stride, &input[0], input.size());
		if (res != 0)
		{
			fprintf(stderr, "Error decoding input: %d\n", res);
			return 2;
		}

		fwrite(decoded.data(), 1, decoded.size(), stdout);
		return 0;
	}
	else if (getenv("V") && atoi(getenv("V")))
	{
		size_t vertex_count = input.size() / stride;
		std::vector<unsigned char> output(input.size() * 4); // todo
		size_t output_size;
		if (vec3 == 2)
			output_size = encodeNDZ<unsigned int>(output.data(), output.size(), input.data(), vertex_count, stride);
		else if (vec3 == 3)
			output_size = encodeNDZ<unsigned short>(output.data(), output.size(), input.data(), vertex_count, stride);
		else if (vec3 == 4)
			output_size = encodeNDZ<unsigned char>(output.data(), output.size(), input.data(), vertex_count, stride);
		else
			output_size = encodeV1(output.data(), output.size(), input.data(), vertex_count, stride);

		fwrite(output.data(), 1, output_size, stdout);
		return 0;
	}
	else
	{
		size_t vertex_count = input.size() / stride;
		std::vector<unsigned char> output(meshopt_encodeVertexBufferBound(vertex_count, stride));
		size_t output_size = meshopt_encodeVertexBuffer(output.data(), output.size(), input.data(), vertex_count, stride);

		fwrite(output.data(), 1, output_size, stdout);
		return 0;
	}
}
