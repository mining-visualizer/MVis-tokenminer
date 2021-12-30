#pragma once

#include <ethminer/secp256k1/secp256k1.h>

using namespace std;
using namespace dev;
using namespace dev::eth;


using Secret = SecureFixedHash<32>;

/// A public key: 64 bytes.
/// @NOTE This is not endian-specific; it's just a bunch of bytes.
using Public = h512;

/// A signature: 65 bytes: r: [0, 32), s: [32, 64), v: 64.
/// @NOTE This is not endian-specific; it's just a bunch of bytes.
/// 
using SigHash = FixedHash<68>;

using Signature = SigHash;

/// Named-boolean type to encode whether a signature be included in the serialisation process.
enum IncludeSignature
{
	WithoutSignature = 0,	///< Do not include a signature.
	WithSignature = 1,		///< Do include a signature.
};

struct SignatureStruct
{
	SignatureStruct() = default;
	SignatureStruct(Signature const& _s) { *(SigHash*)this = _s; }
	SignatureStruct(h256 const& _r, h256 const& _s, byte _v) : r(_r), s(_s), v(_v) {}
	operator Signature() const { return *(SigHash const*)this; }

	/// @returns true if r,s,v values are valid, otherwise false
	bool isValid() const noexcept {
		if (v > 1 ||
			r >= h256("0xfffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141") ||
			s >= h256("0xfffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141") ||
			s < h256(1) ||
			r < h256(1))
			return false;
		return true;
	}

	h256 r;
	h256 s;
	unsigned v = 0;
};

class Secp256k1Context
{
public:
	static secp256k1_context_t const* get() { if (!s_this) s_this = new Secp256k1Context; return s_this->m_ctx; }

private:
	Secp256k1Context() { m_ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY); }
	~Secp256k1Context() { secp256k1_context_destroy(m_ctx); }

	secp256k1_context_t* m_ctx;

	static Secp256k1Context* s_this;
};
Secp256k1Context* Secp256k1Context::s_this = nullptr;


static const u256 c_secp256k1n("115792089237316195423570985008687907852837564279074904382605163141518161494337");

Signature signBytes(Secret const& _k, h256 const& _hash) {
	Signature s;
	SignatureStruct& ss = *reinterpret_cast<SignatureStruct*>(&s);

	int v;
	if (!secp256k1_ecdsa_sign_compact(Secp256k1Context::get(), _hash.data(), s.data(), _k.data(), NULL, NULL, &v))
		return Signature();
	ss.v = v;
	if (ss.s > c_secp256k1n / 2) {
		ss.v = ss.v ^ 1;
		ss.s = h256(c_secp256k1n - u256(ss.s));
	}
	assert(ss.s <= c_secp256k1n / 2);
	return s;
}

class Transaction
{
public:

	// Constructs a null transaction.
	Transaction() {}

	void streamRLP(RLPStream& _s, IncludeSignature _sig) {
		_s.appendList(9);
		_s << nonce << gasPrice << gas;
		_s << receiveAddress;
		_s << value << data;
		if (_sig) {
			_s << m_vrs.v << (u256) m_vrs.r << (u256) m_vrs.s;
		} else {
			_s << chainId << (u256) 0 << (u256) 0;
		}
	}

	/// @returns the RLP serialisation of this transaction.
	bytes rlp(IncludeSignature _sig = WithSignature) { 
		RLPStream s; 
		streamRLP(s, _sig); 
		return s.out(); 
	}

	/// @returns the SHA3 hash of the RLP serialisation of this transaction.
	h256 sha3(IncludeSignature _sig = WithSignature) {
		RLPStream s;
		streamRLP(s, _sig);
		auto ret = dev::sha3(s.out());
		return ret;
	}

	void sign(Secret const& _priv) {
		auto sig = signBytes(_priv, sha3(WithoutSignature));
		SignatureStruct sigStruct = *(SignatureStruct const*) &sig;
		if (sigStruct.isValid()) {
			m_vrs = sigStruct;
			m_vrs.v += chainId * 2 + 35;
		}
	}

	unsigned chainId;
	u256 nonce;						///< The transaction-count of the sender.
	u256 value;						///< The amount of ETH to be transferred by this transaction. Called 'endowment' for contract-creation transactions.
	Address receiveAddress;			///< The receiving address of the transaction.
	u256 gasPrice;					///< The base fee and thus the implied exchange rate of ETH to GAS.
	u256 gas;						///< The total gas to convert, paid for from sender's account. Any unused gas gets refunded once the contract is ended.
	bytes data;						///< The data associated with the transaction, or the initialiser if it's a creation transaction.
	h160 m_sender;					///< Cached sender, determined from signature.
	string txHash;					///< hash of tx
	bytes challenge;				///< challenge this tx was submitted under.

	SignatureStruct m_vrs;			///< The signature of the transaction. Encodes the sender.

};


