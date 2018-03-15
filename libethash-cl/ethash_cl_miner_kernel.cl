#define OPENCL_PLATFORM_UNKNOWN 0
#define OPENCL_PLATFORM_NVIDIA  1
#define OPENCL_PLATFORM_AMD		2

#pragma OPENCL EXTENSION cl_khr_int64_base_atomics : enable 

#ifndef ACCESSES
#define ACCESSES 64
#endif

#ifndef GROUP_SIZE
#define GROUP_SIZE 128		// workgroup size. gets set before kernel is called.
#endif

#ifndef MAX_OUTPUTS
#define MAX_OUTPUTS 63U
#endif

#ifndef PLATFORM
#define PLATFORM 2
#endif

// this is not the byte size, but rather number of 128 byte chunks in the dag file.  Keep in mind that
// dag elements are 64 bytes.  DAG_SIZE = dag_byte_size / 128
#ifndef DAG_SIZE
#define DAG_SIZE 8388593
#endif

#ifndef LIGHT_SIZE
#define LIGHT_SIZE 262139
#endif

#define ETHASH_DATASET_PARENTS 256
#define NODE_WORDS (64/4)

#define THREADS_PER_HASH (128 / 16)			// 8
#define HASHES_PER_LOOP (GROUP_SIZE / THREADS_PER_HASH)
#define FNV_PRIME	0x01000193

typedef struct
{
	uint solutions[MAX_OUTPUTS + 1];
	ulong hashes[2];		// 0=arbitrary hash, 1=best hash
} search_results_t;

__constant uint2 const Keccak_f1600_RC[24] = {
	(uint2)(0x00000001, 0x00000000),
	(uint2)(0x00008082, 0x00000000),
	(uint2)(0x0000808a, 0x80000000),
	(uint2)(0x80008000, 0x80000000),
	(uint2)(0x0000808b, 0x00000000),
	(uint2)(0x80000001, 0x00000000),
	(uint2)(0x80008081, 0x80000000),
	(uint2)(0x00008009, 0x80000000),
	(uint2)(0x0000008a, 0x00000000),
	(uint2)(0x00000088, 0x00000000),
	(uint2)(0x80008009, 0x00000000),
	(uint2)(0x8000000a, 0x00000000),
	(uint2)(0x8000808b, 0x00000000),
	(uint2)(0x0000008b, 0x80000000),
	(uint2)(0x00008089, 0x80000000),
	(uint2)(0x00008003, 0x80000000),
	(uint2)(0x00008002, 0x80000000),
	(uint2)(0x00000080, 0x80000000),
	(uint2)(0x0000800a, 0x00000000),
	(uint2)(0x8000000a, 0x80000000),
	(uint2)(0x80008081, 0x80000000),
	(uint2)(0x00008080, 0x80000000),
	(uint2)(0x80000001, 0x00000000),
	(uint2)(0x80008008, 0x80000000),
};

/*-----------------------------------------------------------------------------------
* copy
*----------------------------------------------------------------------------------*/
#define copy(dst, src, count) for (uint i = 0; i != count; ++i) { (dst)[i] = (src)[i]; }


/*-----------------------------------------------------------------------------------
* ROL2
*----------------------------------------------------------------------------------*/
#if PLATFORM == OPENCL_PLATFORM_NVIDIA && COMPUTE >= 35

static uint2 ROL2(const uint2 a, const int offset) {
	uint2 result;
	if (offset >= 32) {
		asm("shf.l.wrap.b32 %0, %1, %2, %3;" : "=r"(result.x) : "r"(a.x), "r"(a.y), "r"(offset));
		asm("shf.l.wrap.b32 %0, %1, %2, %3;" : "=r"(result.y) : "r"(a.y), "r"(a.x), "r"(offset));
	}
	else {
		asm("shf.l.wrap.b32 %0, %1, %2, %3;" : "=r"(result.x) : "r"(a.y), "r"(a.x), "r"(offset));
		asm("shf.l.wrap.b32 %0, %1, %2, %3;" : "=r"(result.y) : "r"(a.x), "r"(a.y), "r"(offset));
	}
	return result;
}

// this is for r <= 32
static uint2 ROL2_small(const uint2 vv, const int r)
{
	return ROL2(vv, r);
}

// this is for r > 32
static uint2 ROL2_large(const uint2 vv, const int r)
{
	return ROL2(vv, r);
}

#elif PLATFORM == OPENCL_PLATFORM_AMD

#pragma OPENCL EXTENSION cl_amd_media_ops : enable

static uint2 ROL2(const uint2 vv, const int r)
{
	if (r <= 32)
	{
		return amd_bitalign((vv).xy, (vv).yx, 32 - r);
	}
	else
	{
		return amd_bitalign((vv).yx, (vv).xy, 64 - r);
	}
}

// this is for r <= 32
static uint2 ROL2_small(const uint2 vv, const int r)
{
	return amd_bitalign((vv).xy, (vv).yx, 32 - r);
}

// this is for r > 32
static uint2 ROL2_large(const uint2 vv, const int r)
{
	return amd_bitalign((vv).yx, (vv).xy, 64 - r);
}

#else

static uint2 ROL2(const uint2 v, const int n)
{
	uint2 result;
	if (n <= 32)
	{
		result.y = ((v.y << (n)) | (v.x >> (32 - n)));
		result.x = ((v.x << (n)) | (v.y >> (32 - n)));
	}
	else
	{
		result.y = ((v.x << (n - 32)) | (v.y >> (64 - n)));
		result.x = ((v.y << (n - 32)) | (v.x >> (64 - n)));
	}
	return result;
}

// this is for r <= 32
static uint2 ROL2_small(const uint2 vv, const int r)
{
	return ROL2(vv, r);
}

// this is for r > 32
static uint2 ROL2_large(const uint2 vv, const int r)
{
	return ROL2(vv, r);
}

#endif

/*-----------------------------------------------------------------------------------
* chi
*----------------------------------------------------------------------------------*/
static void chi(uint2 * a, const uint n, const uint2 * t)
{
	a[n+0] = bitselect(t[n + 0] ^ t[n + 2], t[n + 0], t[n + 1]);
	a[n+1] = bitselect(t[n + 1] ^ t[n + 3], t[n + 1], t[n + 2]);
	a[n+2] = bitselect(t[n + 2] ^ t[n + 4], t[n + 2], t[n + 3]);
	a[n+3] = bitselect(t[n + 3] ^ t[n + 0], t[n + 3], t[n + 4]);
	a[n+4] = bitselect(t[n + 4] ^ t[n + 1], t[n + 4], t[n + 0]);
}


/*-----------------------------------------------------------------------------------
* keccak_f1600_round
*----------------------------------------------------------------------------------*/
static void keccak_f1600_round(uint2* a, uint r)
{
	uint2 t[25];
	uint2 u;

	// Theta
	t[0] = a[0] ^ a[5] ^ a[10] ^ a[15] ^ a[20];
	t[1] = a[1] ^ a[6] ^ a[11] ^ a[16] ^ a[21];
	t[2] = a[2] ^ a[7] ^ a[12] ^ a[17] ^ a[22];
	t[3] = a[3] ^ a[8] ^ a[13] ^ a[18] ^ a[23];
	t[4] = a[4] ^ a[9] ^ a[14] ^ a[19] ^ a[24];
	u = t[4] ^ ROL2_small(t[1], 1);
	a[0] ^= u;
	a[5] ^= u;
	a[10] ^= u;
	a[15] ^= u;
	a[20] ^= u;
	u = t[0] ^ ROL2_small(t[2], 1);
	a[1] ^= u;
	a[6] ^= u;
	a[11] ^= u;
	a[16] ^= u;
	a[21] ^= u;
	u = t[1] ^ ROL2_small(t[3], 1);
	a[2] ^= u;
	a[7] ^= u;
	a[12] ^= u;
	a[17] ^= u;
	a[22] ^= u;
	u = t[2] ^ ROL2_small(t[4], 1);
	a[3] ^= u;
	a[8] ^= u;
	a[13] ^= u;
	a[18] ^= u;
	a[23] ^= u;
	u = t[3] ^ ROL2_small(t[0], 1);
	a[4] ^= u;
	a[9] ^= u;
	a[14] ^= u;
	a[19] ^= u;
	a[24] ^= u;

	// Rho Pi

	t[0]  = a[0];
	t[10] = ROL2_small(a[1], 1);
	t[20] = ROL2_large(a[2], 62);
	t[5]  = ROL2_small(a[3], 28);
	t[15] = ROL2_small(a[4], 27);
	
	t[16] = ROL2_large(a[5], 36);
	t[1]  = ROL2_large(a[6], 44);
	t[11] = ROL2_small(a[7], 6);
	t[21] = ROL2_large(a[8], 55);
	t[6]  = ROL2_small(a[9], 20);
	
	t[7]  = ROL2_small(a[10], 3);
	t[17] = ROL2_small(a[11], 10);
	t[2]  = ROL2_large(a[12], 43);
	t[12] = ROL2_small(a[13], 25);
	t[22] = ROL2_large(a[14], 39);
	
	t[23] = ROL2_large(a[15], 41);
	t[8]  = ROL2_large(a[16], 45);
	t[18] = ROL2_small(a[17], 15);
	t[3]  = ROL2_small(a[18], 21);
	t[13] = ROL2_small(a[19], 8);
	
	t[14] = ROL2_small(a[20], 18);
	t[24] = ROL2_small(a[21], 2);
	t[9]  = ROL2_large(a[22], 61);
	t[19] = ROL2_large(a[23], 56);
	t[4]  = ROL2_small(a[24], 14);

	// Chi
	chi(a, 0, t);

	// Iota
	a[0] ^= Keccak_f1600_RC[r];

	chi(a, 5, t);
	chi(a, 10, t);
	chi(a, 15, t);
	chi(a, 20, t);
}

/*-----------------------------------------------------------------------------------
* keccak_f1600_reduced
*----------------------------------------------------------------------------------*/
static void keccak_f1600_reduced(uint2* state, uint r)
{
	uint2 t[11];
	uint2 u;

	// Theta
	t[0] = state[0] ^ state[5] ^ state[10] ^ state[15] ^ state[20];
	t[1] = state[1] ^ state[6] ^ state[11] ^ state[16] ^ state[21];
	t[2] = state[2] ^ state[7] ^ state[12] ^ state[17] ^ state[22];
	t[3] = state[3] ^ state[8] ^ state[13] ^ state[18] ^ state[23];
	t[4] = state[4] ^ state[9] ^ state[14] ^ state[19] ^ state[24];
	u = t[4] ^ ROL2_small(t[1], 1);
	state[0] ^= u;
	state[5] ^= u;
	state[10] ^= u;
	state[15] ^= u;
	state[20] ^= u;
	u = t[0] ^ ROL2_small(t[2], 1);
	state[1] ^= u;
	state[6] ^= u;
	state[11] ^= u;
	state[16] ^= u;
	state[21] ^= u;
	u = t[1] ^ ROL2_small(t[3], 1);
	state[2] ^= u;
	state[7] ^= u;
	state[12] ^= u;
	state[17] ^= u;
	state[22] ^= u;
	u = t[2] ^ ROL2_small(t[4], 1);
	state[3] ^= u;
	state[8] ^= u;
	state[13] ^= u;
	state[18] ^= u;
	state[23] ^= u;
	u = t[3] ^ ROL2_small(t[0], 1);
	state[4] ^= u;
	state[9] ^= u;
	state[14] ^= u;
	state[19] ^= u;
	state[24] ^= u;


	// ====== Rho Pi ======

	// see "keccak permutations.docx" for an explanation of all this.


	uint t_map[25];

	t[0] = state[0];
	t[1] = ROL2_large(state[6], 44);
	t[2] = ROL2_large(state[12], 43);
	t[3] = ROL2_small(state[18], 21);
	t[4] = ROL2_small(state[24], 14);
	t[5] = ROL2_small(state[3], 28);		// t[5]
	t[6] = ROL2_small(state[1], 1);			// t[10]
	t[7] = ROL2_small(state[4], 27);		// t[15]
	t[8] = ROL2_large(state[2], 62);		// t[20]

											// t[0:8] are in use

	t_map[5] = 5;
	t_map[10] = 6;
	t_map[15] = 7;
	t_map[20] = 8;

	state[0] = bitselect(t[0] ^ t[2], t[0], t[1]);
	state[1] = bitselect(t[1] ^ t[3], t[1], t[2]);
	state[2] = bitselect(t[2] ^ t[4], t[2], t[3]);
	state[3] = bitselect(t[3] ^ t[0], t[3], t[4]);
	state[4] = bitselect(t[4] ^ t[1], t[4], t[0]);

	// releasing t[0:4]
	// t[0:4] are free, t[5:8] are in use

	t[0] = ROL2_small(state[9], 20);		// t[6]
	t[1] = ROL2_small(state[10], 3);		// t[7]
	t[2] = ROL2_large(state[16], 45);		// t[8]
	t[3] = ROL2_large(state[22], 61);		// t[9]
	t[4] = ROL2_small(state[7], 6);			// t[11]
	t[9] = ROL2_large(state[5], 36);		// t[16]
	t[10] = ROL2_large(state[8], 55);		// t[21]

											// t[0:10] are in use

	t_map[6] = 0;
	t_map[7] = 1;
	t_map[8] = 2;
	t_map[9] = 3;
	t_map[11] = 4;
	t_map[16] = 9;
	t_map[21] = 10;

	// using t[0:3, 5] for following calculations
	state[5] = bitselect(t[t_map[5]] ^ t[t_map[7]], t[t_map[5]], t[t_map[6]]);
	state[6] = bitselect(t[t_map[6]] ^ t[t_map[8]], t[t_map[6]], t[t_map[7]]);
	state[7] = bitselect(t[t_map[7]] ^ t[t_map[9]], t[t_map[7]], t[t_map[8]]);
	state[8] = bitselect(t[t_map[8]] ^ t[t_map[5]], t[t_map[8]], t[t_map[9]]);
	state[9] = bitselect(t[t_map[9]] ^ t[t_map[6]], t[t_map[9]], t[t_map[5]]);

	// releasing t[0:3, 5]
	// t[0:3, 5] are free, t[4, 6:10] are in use

	t[0] = ROL2_small(state[13], 25);		// t[12]
	t[1] = ROL2_small(state[19], 8);		// t[13]
	t[2] = ROL2_small(state[20], 18);		// t[14]
	t[3] = ROL2_small(state[11], 10);		// t[17]
	t[5] = ROL2_large(state[14], 39);		// t[22]

	t_map[12] = 0;
	t_map[13] = 1;
	t_map[14] = 2;
	t_map[17] = 3;
	t_map[22] = 5;

	// t[0:10] are in use

	// using t[0:2,4,6] for following calculations
	state[10] = bitselect(t[t_map[10]] ^ t[t_map[12]], t[t_map[10]], t[t_map[11]]);
	state[11] = bitselect(t[t_map[11]] ^ t[t_map[13]], t[t_map[11]], t[t_map[12]]);
	state[12] = bitselect(t[t_map[12]] ^ t[t_map[14]], t[t_map[12]], t[t_map[13]]);
	state[13] = bitselect(t[t_map[13]] ^ t[t_map[10]], t[t_map[13]], t[t_map[14]]);
	state[14] = bitselect(t[t_map[14]] ^ t[t_map[11]], t[t_map[14]], t[t_map[10]]);

	// releasing t[0:2,4,6]
	// t[0:2,4,6] are free, t[3,5,7:10] are in use

	t[0] = ROL2_small(state[17], 15);		// t[18]
	t[1] = ROL2_large(state[23], 56);		// t[19]
	t[2] = ROL2_large(state[15], 41);		// t[23]

											// t[4,6] are free, t[0:2,5,7:10] are in use

	t_map[18] = 0;
	t_map[19] = 1;
	t_map[23] = 2;

	// using t[7,9,3,0,1] for following calculations
	state[15] = bitselect(t[t_map[15]] ^ t[t_map[17]], t[t_map[15]], t[t_map[16]]);
	state[16] = bitselect(t[t_map[16]] ^ t[t_map[18]], t[t_map[16]], t[t_map[17]]);
	state[17] = bitselect(t[t_map[17]] ^ t[t_map[19]], t[t_map[17]], t[t_map[18]]);
	state[18] = bitselect(t[t_map[18]] ^ t[t_map[15]], t[t_map[18]], t[t_map[19]]);
	state[19] = bitselect(t[t_map[19]] ^ t[t_map[16]], t[t_map[19]], t[t_map[15]]);

	// releasing t[7,9,3,0,1]
	// t[4,6,7,9,3,0,1] are free, t[2,5,8,10] are in use

	t[0] = ROL2_small(state[21], 2);		// t[24]

	t_map[24] = 0;

	state[20] = bitselect(t[t_map[20]] ^ t[t_map[22]], t[t_map[20]], t[t_map[21]]);
	state[21] = bitselect(t[t_map[21]] ^ t[t_map[23]], t[t_map[21]], t[t_map[22]]);
	state[22] = bitselect(t[t_map[22]] ^ t[t_map[24]], t[t_map[22]], t[t_map[23]]);
	state[23] = bitselect(t[t_map[23]] ^ t[t_map[20]], t[t_map[23]], t[t_map[24]]);
	state[24] = bitselect(t[t_map[24]] ^ t[t_map[21]], t[t_map[24]], t[t_map[20]]);

	// Iota
	state[0] ^= Keccak_f1600_RC[r];
}


/*-----------------------------------------------------------------------------------
* keccak_f1600_no_absorb
*----------------------------------------------------------------------------------*/
static void keccak_f1600_no_absorb(uint2* a, uint out_size, uint isolate)
{
	
	for (uint r = 0; r < 24;)
	{
		// This dynamic branch stops the AMD compiler unrolling the loop
		// and additionally saves about 33% of the VGPRs, enough to gain another
		// wavefront. Ideally we'd get 4 in flight, but 3 is the best I can
		// massage out of the compiler. It doesn't really seem to matter how
		// much we try and help the compiler save VGPRs because it seems to throw
		// that information away, hence the implementation of keccak here
		// doesn't bother.
		if (isolate)
		{
			keccak_f1600_round(a, r++);
			//keccak_f1600_reduced(a, r++);
		}
	} 
}

/*-----------------------------------------------------------------------------------
* fnv
*----------------------------------------------------------------------------------*/
static uint fnv(uint x, uint y)
{
	return x * FNV_PRIME ^ y;
}

/*-----------------------------------------------------------------------------------
* fnv2
*----------------------------------------------------------------------------------*/
static uint2 fnv2(uint2 x, uint2 y)
{
	return x * FNV_PRIME ^ y;
}

/*-----------------------------------------------------------------------------------
* fnv4
*----------------------------------------------------------------------------------*/
static uint4 fnv4(uint4 x, uint4 y)
{
	return x * FNV_PRIME ^ y;
}

/*-----------------------------------------------------------------------------------
* fnv_reduce
*----------------------------------------------------------------------------------*/
static uint fnv_reduce(uint4 v)
{
	return fnv(fnv(fnv(v.x, v.y), v.z), v.w);
}

typedef struct
{
	ulong ulongs[20 / sizeof(ulong)];
} hash20_t;

typedef struct
{
	ulong ulongs[32 / sizeof(ulong)];
} hash32_t;

typedef union {
	uint	 words[64 / sizeof(uint)];
	uint2	 uint2s[64 / sizeof(uint2)];
	uint4	 uint4s[64 / sizeof(uint4)];
} hash64_t;

typedef union {
	uchar	 uchars[200 / sizeof(uchar)];
	uint	 words[200 / sizeof(uint)];
	uint2	 uint2s[200 / sizeof(uint2)];
	uint4	 uint4s[200 / sizeof(uint4)];
	ulong	 ulongs[200 / sizeof(ulong)];
} hash200_t;

typedef union
{
	uint2 uint2s[128 / sizeof(uint2)];
	uint4 uint4s[128 / sizeof(uint4)];
} hash128_t;

typedef union {
	uint2 uint2s[8];
	uint4 uint4s[4];
	ulong ulongs[8];
	uint  uints[16];
} compute_hash_share;

/*----------------------------------------------------------------------------------
* compute_hash
*----------------------------------------------------------------------------------*/
static hash32_t compute_hash(
	__constant hash32_t const* g_header,
	__global hash128_t const* g_dag,
	ulong nonce,
	uint isolate
)
{
	// HASHES_PER_LOOP = workgroup_size / 8
	__local compute_hash_share share[HASHES_PER_LOOP];

	uint const gid = get_global_id(0);

	// Compute one init hash per work item.

	// sha3_512(header .. nonce)
	ulong state[25];
	copy(state, g_header->ulongs, 4);
	state[4] = nonce;

	for (uint i = 6; i != 25; ++i)
	{
		state[i] = 0;
	}
	state[5] = 0x0000000000000001;
	state[8] = 0x8000000000000000;

	keccak_f1600_no_absorb((uint2*) state, 8, isolate);

	// Threads work together in this phase in groups of 8.
	uint const thread_id = gid & 7;
	uint const hash_id = get_local_id(0) >> 3;

	for (int i = 0; i < THREADS_PER_HASH; i++)
	{
		// share init with other threads
		if (i == thread_id)
			copy(share[hash_id].ulongs, state, 8);

		barrier(CLK_LOCAL_MEM_FENCE);

		uint4 mix = share[hash_id].uint4s[thread_id & 3];
		__local uint *share0 = share[hash_id].uints;
		uint init0 = share[hash_id].uints[0];
		barrier(CLK_LOCAL_MEM_FENCE);

		for (uint a = 0; a < ACCESSES; a += 4)
		{
			bool update_share = thread_id == ((a >> 2) & (THREADS_PER_HASH - 1));

			for (uint i = 0; i != 4; ++i)
			{
				if (update_share)
				{
					*share0 = fnv(init0 ^ (a + i), ((uint *) &mix)[i]) % DAG_SIZE;
				}
				barrier(CLK_LOCAL_MEM_FENCE);

				mix = fnv4(mix, g_dag[*share0].uint4s[thread_id]);
			}
		}

		share[hash_id].uints[thread_id] = fnv_reduce(mix);
		barrier(CLK_LOCAL_MEM_FENCE);

		if (i == thread_id)
			copy(state + 8, share[hash_id].ulongs, 4);

		barrier(CLK_LOCAL_MEM_FENCE);
	}

	for (uint i = 13; i != 25; ++i)
	{
		state[i] = 0;
	}
	state[12] = 0x0000000000000001;
	state[16] = 0x8000000000000000;

	// keccak_256(keccak_512(header..nonce) .. mix);
	keccak_f1600_no_absorb((uint2*) state, 1, isolate);

	return *(hash32_t*) &state[0];
}

/*-----------------------------------------------------------------------------------
* test_keccak  
*----------------------------------------------------------------------------------*/
#if PLATFORM != OPENCL_PLATFORM_NVIDIA // use maxrregs on nv
__attribute__((reqd_work_group_size(GROUP_SIZE, 1, 1)))
#endif
__kernel void test_keccak(
	__constant uchar const* g_challenge,		// 32 bytes	
	__constant uint const* g_sender,			// 20 bytes (5 uints)
	__constant uint const* g_nonce,				// 32 bytes (8 uints)
	__global volatile uint* restrict g_output,	// 32 bytes (8 uints)
	uint isolate
) {
	uint const gid = get_global_id(0);
	if (gid != 6) return;

	hash200_t state;

	copy(state.uchars, g_challenge, 32);
	copy(state.words + 8, g_sender, 5);
	copy(state.words + 13, g_nonce, 8);
	state.words[13] = gid;

	for (uint i = 21; i != 50; ++i) {
		state.words[i] = 0;
	}

	state.uchars[84] = 0x01;
	state.uchars[135] = 0x80;

	// keccak_256
	keccak_f1600_no_absorb((uint2*) &state, 1, isolate);
	copy(g_output, state.words, 8);
}

/*-----------------------------------------------------------------------------------
* 0xbitcoin_search
*----------------------------------------------------------------------------------*/
#if PLATFORM != OPENCL_PLATFORM_NVIDIA // use maxrregs on nv
__attribute__((reqd_work_group_size(GROUP_SIZE, 1, 1)))
#endif
__kernel void bitcoin0x_search(
	__constant uchar const* g_challenge,		// 32 bytes	
	__constant uint const* g_sender,			// 20 bytes (5 uints)
	__constant uint const* g_nonce,				// 32 bytes (8 uints)
	__global volatile search_results_t* restrict g_output,	
	ulong target,
	uint isolate,
	__global volatile hash200_t* restrict g_buff		// 200 bytes,	used for debugging
	) 
{
	uint const gid = get_global_id(0);

	hash200_t state;
	copy(state.uchars, g_challenge, 32);
	copy(state.words + 8, g_sender, 5);
	copy(state.words + 13, g_nonce, 8);
	// overwrite the upper 4 bytes of the nonce with the work item #, that way every
	// work item is computing for a different nonce.
	state.words[13] = gid;

	for (uint i = 21; i != 50; ++i) {
		state.words[i] = 0;
	}

	// keccak_256
	state.uchars[84] = 0x01;
	state.uchars[135] = 0x80;
	keccak_f1600_no_absorb((uint2*) &state, 1, isolate);
	
	// pick off upper 64 bits of hash
	__private ulong ulhash = as_ulong(as_uchar8(state.ulongs[0]).s76543210);

	if (ulhash < target) {
		uint slot = min(MAX_OUTPUTS, atomic_inc(&g_output->solutions[0]) + 1);
		g_output->solutions[slot] = gid;
	}


}


/*-----------------------------------------------------------------------------------
* ethash_search
*----------------------------------------------------------------------------------*/
#if PLATFORM != OPENCL_PLATFORM_NVIDIA // use maxrregs on nv
__attribute__((reqd_work_group_size(GROUP_SIZE, 1, 1)))
#endif
__kernel void ethash_search(
	__global volatile search_results_t* restrict g_output,
	__constant hash32_t const* g_header,
	__global hash128_t const* g_dag,
	__global volatile ulong* restrict g_bestHash,
	ulong start_nonce,
	ulong target,
	ulong threshold,
	uint isolate
	)
{
	uint const gid = get_global_id(0);
	hash32_t hash = compute_hash(g_header, g_dag, start_nonce + gid, isolate);
	__private ulong ulhash = as_ulong(as_uchar8(hash.ulongs[0]).s76543210);

	if (gid == 0)
		g_output->hashes[0] = ulhash;		// just an arbitrary hash for the GUI app to display

	// threshold is set by the host to be the greater of *g_bestHash and target
	if (ulhash < threshold)
	{
		if (ulhash < *g_bestHash)
		{
			// g_bestHash is for us.  it is preserved across kernel invocations.  
			// g_output->hashes[1] is for our caller, so they can know the latest best hash value.
			atom_xchg(g_bestHash, ulhash);
			atom_xchg(&g_output->hashes[1], ulhash);

		}
		if (ulhash < target)
		{
			uint slot = min(MAX_OUTPUTS, atomic_inc(&g_output->solutions[0]) + 1);
			g_output->solutions[slot] = gid;
		}
	}
}


/*-----------------------------------------------------------------------------------
* ethash_hash  -  generate hashes and return them in their entirety.
*----------------------------------------------------------------------------------*/
#if PLATFORM != OPENCL_PLATFORM_NVIDIA // use maxrregs on nv
__attribute__((reqd_work_group_size(GROUP_SIZE, 1, 1)))
#endif
__kernel void ethash_hash(
	__global volatile hash32_t* restrict g_output,
	__constant hash32_t const* g_header,
	__global hash128_t const* g_dag,
	ulong start_nonce,
	uint isolate
)
{
	uint const gid = get_global_id(0);
	g_output[gid] = compute_hash(g_header, g_dag, start_nonce + gid, isolate);

	// just put some regular patterns in for testing
	//__global short* b = (__global short*)(g_output + gid);
	//for (int i = 0; i < 16; i++)
	//	b[i] = i + gid;

}

/*-----------------------------------------------------------------------------------
* SHA3_512
*----------------------------------------------------------------------------------*/
static void SHA3_512(uint2* s, uint isolate)
{
	for (uint i = 8; i != 25; ++i)
	{
		s[i] = (uint2){ 0, 0 };
	}
	s[8].x = 0x00000001;
	s[8].y = 0x80000000;
	keccak_f1600_no_absorb(s, 8, isolate);
}


/*-----------------------------------------------------------------------------------
* keccak
*----------------------------------------------------------------------------------*/
__kernel void keccak(__global volatile ulong* g_data, uint isolate)
{
	// input data should contain 8 ulongs (64 bytes), 5 of which will be hashed.
	// we're mimicking the main hashing algorithm here.

	ulong state[25];

	uint const gid = get_global_id(0);
	copy(state, g_data + (gid * 8), 5);

	for (uint i = 6; i != 25; ++i)
	{
		state[i] = 0;
	}
	state[5] = 0x0000000000000001;
	state[8] = 0x8000000000000000;

	keccak_f1600_no_absorb((uint2*) state, 0, isolate);
	copy(g_data + (gid * 8), state, 8);
}
