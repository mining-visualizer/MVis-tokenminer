#define OPENCL_PLATFORM_UNKNOWN 0
#define OPENCL_PLATFORM_NVIDIA  1
#define OPENCL_PLATFORM_AMD		2

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


typedef union
{
	uchar	 uchars[200 / sizeof(uchar)];
	uint	 words[200 / sizeof(uint)];
	uint2	 uint2s[200 / sizeof(uint2)];
	uint4	 uint4s[200 / sizeof(uint4)];
	ulong	 ulongs[200 / sizeof(ulong)];
} hash200_t;


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
* chi5
*----------------------------------------------------------------------------------*/
static void chi5(uint2 * a, const uint n, const uint2 * t)
{
	a[n+0] = bitselect(t[n + 0] ^ t[n + 2], t[n + 0], t[n + 1]);
	a[n+1] = bitselect(t[n + 1] ^ t[n + 3], t[n + 1], t[n + 2]);
	a[n+2] = bitselect(t[n + 2] ^ t[n + 4], t[n + 2], t[n + 3]);
	a[n+3] = bitselect(t[n + 3] ^ t[n + 0], t[n + 3], t[n + 4]);
	a[n+4] = bitselect(t[n + 4] ^ t[n + 1], t[n + 4], t[n + 0]);
}

/*-----------------------------------------------------------------------------------
* chi
*----------------------------------------------------------------------------------*/
static uint2 chi(uint2 a, uint2 b, uint2 c)
{
	return bitselect(a ^ c, a, b);
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
	chi5(a, 0, t);

	// Iota
	a[0] ^= Keccak_f1600_RC[r];

	chi5(a, 5, t);
	chi5(a, 10, t);
	chi5(a, 15, t);
	chi5(a, 20, t);
}

/*-----------------------------------------------------------------------------------
*                keccak_final_round
*----------------------------------------------------------------------------------*/
static void keccak_final_round(uint2* a)
{
	uint2 t[5];

	t[0] = a[0] ^ a[5] ^ a[10] ^ a[15] ^ a[20];
	t[1] = a[1] ^ a[6] ^ a[11] ^ a[16] ^ a[21];
	t[2] = a[2] ^ a[7] ^ a[12] ^ a[17] ^ a[22];
	t[3] = a[3] ^ a[8] ^ a[13] ^ a[18] ^ a[23];
	t[4] = a[4] ^ a[9] ^ a[14] ^ a[19] ^ a[24];

	a[0] ^= ROL2_small(t[1], 1) ^ t[4];
	a[6] ^= ROL2_small(t[2], 1) ^ t[0];
	a[12] ^= ROL2_small(t[3], 1) ^ t[1];

	a[1] = ROL2_large(a[6], 44);
	a[2] = ROL2_large(a[12], 43);

	a[0] = chi(a[0], a[1], a[2]);

	a[0] ^= Keccak_f1600_RC[23];
}


/*-----------------------------------------------------------------------------------
* keccak_f1600_no_absorb
*----------------------------------------------------------------------------------*/
static void keccak_f1600_no_absorb(uint2* a, uint rounds)
{
	
	for (uint r = 0; r < rounds;)
	{
		keccak_f1600_round(a, r++);
	}
}

/*-----------------------------------------------------------------------------------
* keccak_first_round
*----------------------------------------------------------------------------------*/
static void keccak_first_round(uint2* state, __constant uint2 const* d_mid, uint2 const gid)
{

	uint2 C[5];

	state[2] = d_mid[2] ^ ROL2(gid, 44);
	state[4] = d_mid[4] ^ ROL2(gid, 14);

	state[6] = d_mid[6] ^ ROL2(gid, 20);
	state[9] = d_mid[9] ^ ROL2(gid, 62);

	state[11] = d_mid[11] ^ ROL2(gid, 7);
	state[13] = d_mid[13] ^ ROL2(gid, 8);

	state[15] = d_mid[15] ^ ROL2(gid, 27);
	state[18] = d_mid[18] ^ ROL2(gid, 16);

	state[20] = d_mid[20] ^ ROL2(gid, 63);
	state[21] = d_mid[21] ^ ROL2(gid, 55);
	state[22] = d_mid[22] ^ ROL2(gid, 39);

	//  Chi
	state[0] = chi(d_mid[0], d_mid[1], state[2]);
	state[0] ^= Keccak_f1600_RC[0];
	state[1] = chi(d_mid[1], state[2], d_mid[3]);
	state[2] = chi(state[2], d_mid[3], state[4]);
	state[3] = chi(d_mid[3], state[4], d_mid[0]);
	state[4] = chi(state[4], d_mid[0], d_mid[1]);

	C[0] = state[6];
	state[5] = chi(d_mid[5], C[0], d_mid[7]);
	state[6] = chi(C[0], d_mid[7], d_mid[8]);
	state[7] = chi(d_mid[7], d_mid[8], state[9]);
	state[8] = chi(d_mid[8], state[9], d_mid[5]);
	state[9] = chi(state[9], d_mid[5], C[0]);

	C[0] = state[11];
	state[10] = chi(d_mid[10], C[0], d_mid[12]);
	state[11] = chi(C[0], d_mid[12], state[13]);
	state[12] = chi(d_mid[12], state[13], d_mid[14]);
	state[13] = chi(state[13], d_mid[14], d_mid[10]);
	state[14] = chi(d_mid[14], d_mid[10], C[0]);

	C[0] = state[15];
	state[15] = chi(C[0], d_mid[16], d_mid[17]);
	state[16] = chi(d_mid[16], d_mid[17], state[18]);
	state[17] = chi(d_mid[17], state[18], d_mid[19]);
	state[18] = chi(state[18], d_mid[19], C[0]);
	state[19] = chi(d_mid[19], C[0], d_mid[16]);

	C[0] = state[20];
	C[1] = state[21];
	state[20] = chi(C[0], C[1], state[22]);
	state[21] = chi(C[1], state[22], d_mid[23]);
	state[22] = chi(state[22], d_mid[23], d_mid[24]);
	state[23] = chi(d_mid[23], d_mid[24], C[0]);
	state[24] = chi(d_mid[24], C[0], C[1]);

}

void keccak_2(uint2* state, uint first_round, uint rounds)
{
	uint2 C[5], D[5];


	for (uint i = first_round; i < rounds; ++i)
	{
		C[0] = state[0] ^ state[5] ^ state[10] ^ state[15] ^ state[20];
		C[1] = state[1] ^ state[6] ^ state[11] ^ state[16] ^ state[21];
		C[2] = state[2] ^ state[7] ^ state[12] ^ state[17] ^ state[22];
		C[3] = state[3] ^ state[8] ^ state[13] ^ state[18] ^ state[23];
		C[4] = state[4] ^ state[9] ^ state[14] ^ state[19] ^ state[24];

#if false
		D[0] = ROL2(C[1], 1) ^ C[4];
		D[1] = ROL2(C[2], 1) ^ C[0];
		D[2] = ROL2(C[3], 1) ^ C[1];
		D[3] = ROL2(C[4], 1) ^ C[2];
		D[4] = ROL2(C[0], 1) ^ C[3];

		for (x = 0; x < 5; x++)
		{
			state[x] ^= D[x];
			state[x + 5] ^= D[x];
			state[x + 10] ^= D[x];
			state[x + 15] ^= D[x];
			state[x + 20] ^= D[x];
		}
#else
		//  this is basically a unrolled version of the above code
		D[0] = ROL2(C[1], 1) ^ C[4];
		state[0] ^= D[0];
		state[5] ^= D[0];
		state[10] ^= D[0];
		state[15] ^= D[0];
		state[20] ^= D[0];

		D[0] = ROL2(C[2], 1) ^ C[0];
		state[1] ^= D[0];
		state[6] ^= D[0];
		state[11] ^= D[0];
		state[16] ^= D[0];
		state[21] ^= D[0];

		D[0] = ROL2(C[3], 1) ^ C[1];
		state[2] ^= D[0];
		state[7] ^= D[0];
		state[12] ^= D[0];
		state[17] ^= D[0];
		state[22] ^= D[0];

		D[0] = ROL2(C[4], 1) ^ C[2];
		state[3] ^= D[0];
		state[8] ^= D[0];
		state[13] ^= D[0];
		state[18] ^= D[0];
		state[23] ^= D[0];

		D[0] = ROL2(C[0], 1) ^ C[3];
		state[4] ^= D[0];
		state[9] ^= D[0];
		state[14] ^= D[0];
		state[19] ^= D[0];
		state[24] ^= D[0];
#endif

		C[0] = state[1];
		state[1] = ROL2(state[6], 44);
		state[6] = ROL2(state[9], 20);
		state[9] = ROL2(state[22], 61);
		state[22] = ROL2(state[14], 39);
		state[14] = ROL2(state[20], 18);
		state[20] = ROL2(state[2], 62);
		state[2] = ROL2(state[12], 43);
		state[12] = ROL2(state[13], 25);
		state[13] = ROL2(state[19], 8);
		state[19] = ROL2(state[23], 56);
		state[23] = ROL2(state[15], 41);
		state[15] = ROL2(state[4], 27);
		state[4] = ROL2(state[24], 14);
		state[24] = ROL2(state[21], 2);
		state[21] = ROL2(state[8], 55);
		state[8] = ROL2(state[16], 45);
		state[16] = ROL2(state[5], 36);
		state[5] = ROL2(state[3], 28);
		state[3] = ROL2(state[18], 21);
		state[18] = ROL2(state[17], 15);
		state[17] = ROL2(state[11], 10);
		state[11] = ROL2(state[7], 6);
		state[7] = ROL2(state[10], 3);
		state[10] = ROL2(C[0], 1);

		C[0] = state[0];
		C[1] = state[1];
		state[0] = chi(state[0], state[1], state[2]);
		state[0] ^= Keccak_f1600_RC[i];
		state[1] = chi(state[1], state[2], state[3]);
		state[2] = chi(state[2], state[3], state[4]);
		state[3] = chi(state[3], state[4], C[0]);
		state[4] = chi(state[4], C[0], C[1]);

		C[0] = state[5];
		C[1] = state[6];
		state[5] = chi(state[5], state[6], state[7]);
		state[6] = chi(state[6], state[7], state[8]);
		state[7] = chi(state[7], state[8], state[9]);
		state[8] = chi(state[8], state[9], C[0]);
		state[9] = chi(state[9], C[0], C[1]);

		C[0] = state[10];
		C[1] = state[11];
		state[10] = chi(state[10], state[11], state[12]);
		state[11] = chi(state[11], state[12], state[13]);
		state[12] = chi(state[12], state[13], state[14]);
		state[13] = chi(state[13], state[14], C[0]);
		state[14] = chi(state[14], C[0], C[1]);

		C[0] = state[15];
		C[1] = state[16];
		state[15] = chi(state[15], state[16], state[17]);
		state[16] = chi(state[16], state[17], state[18]);
		state[17] = chi(state[17], state[18], state[19]);
		state[18] = chi(state[18], state[19], C[0]);
		state[19] = chi(state[19], C[0], C[1]);

		C[0] = state[20];
		C[1] = state[21];
		state[20] = chi(state[20], state[21], state[22]);
		state[21] = chi(state[21], state[22], state[23]);
		state[22] = chi(state[22], state[23], state[24]);
		state[23] = chi(state[23], state[24], C[0]);
		state[24] = chi(state[24], C[0], C[1]);

	}

}



/*-----------------------------------------------------------------------------------
* test_keccak  
*----------------------------------------------------------------------------------*/
#if PLATFORM != OPENCL_PLATFORM_NVIDIA // use maxrregs on nv
__attribute__((reqd_work_group_size(GROUP_SIZE, 1, 1)))
#endif
__kernel void test_keccak(
	__constant uint2 const* g_preCompute,		// 200 bytes
	__global volatile uint2* restrict g_output,	// 8 bytes
	uint isolate
	) {

	ulong const gid = get_global_id(0);

	//  only 1 work item will actually compute a hash, since we only have space in the output buffer for 1 hash
	if (gid != isolate)
		return;

	uint2 gid2;
	gid2.y = (gid >> 32) & 0xffffffff;
	gid2.x = gid & 0xffffffff;

	uint2 state[25];

	keccak_first_round(state, g_preCompute, gid2);

	keccak_2(state, 1, 24);
	//keccak_final_round(state);

	*g_output = state[0];
}

/*-----------------------------------------------------------------------------------
* 0xbitcoin_search
*----------------------------------------------------------------------------------*/
#if PLATFORM != OPENCL_PLATFORM_NVIDIA // use maxrregs on nv
__attribute__((reqd_work_group_size(GROUP_SIZE, 1, 1)))
#endif
__kernel void bitcoin0x_search(
	__constant uint2 const* g_preCompute,			// 200 bytes
	__global volatile uint* restrict g_output,	
	ulong target
)
{
	ulong const gid = get_global_id(0);

	hash200_t state;

	uint2 gid2;
	gid2.y = (gid >> 32) & 0xffffffff;
	gid2.x = gid & 0xffffffff;

	keccak_first_round(state.uint2s, g_preCompute, gid2);

	keccak_2(state.uint2s, 1, 24);
	//keccak_final_round(state.uint2s);

	// pick off upper 64 bits of hash and flip the bytes
	__private ulong ulhash = as_ulong(as_uchar8(state.ulongs[0]).s76543210);

	if (ulhash < target) {
		uint slot = min(MAX_OUTPUTS, atomic_inc(&g_output[0]) + 1);
		g_output[slot] = gid;
	}


}

