/*
 * Copyright (C) 2018 Tobias Brunner
 * HSR Hochschule fuer Technik Rapperswil
 *
 * Copyright (C) 2018 René Korthaus
 * Rohde & Schwarz Cybersecurity GmbH
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "botan_rsa_public_key.h"

#include <botan/build.h>

#ifdef BOTAN_HAS_RSA

#include "botan_util.h"

#include <asn1/oid.h>
#include <asn1/asn1.h>
#include <asn1/asn1_parser.h>

#include <utils/debug.h>

#include <botan/ffi.h>

typedef struct private_botan_rsa_public_key_t private_botan_rsa_public_key_t;

/**
 * Private data structure with signing context.
 */
struct private_botan_rsa_public_key_t {

	/**
	 * Public interface for this signer
	 */
	botan_rsa_public_key_t public;

	/**
	 * Botan public key
	 */
	botan_pubkey_t key;

	/**
	 * Reference counter
	 */
	refcount_t ref;
};

/**
 * Verify RSA signature
 */
static bool verify_rsa_signature(private_botan_rsa_public_key_t *this,
								 const char* hash_and_padding, chunk_t data,
								 chunk_t signature)
{
	botan_pk_op_verify_t verify_op;
	bool valid = FALSE;

	if (botan_pk_op_verify_create(&verify_op, this->key, hash_and_padding, 0))
	{
		return FALSE;
	}

	if (botan_pk_op_verify_update(verify_op, data.ptr, data.len))
	{
		botan_pk_op_verify_destroy(verify_op);
		return FALSE;
	}

	valid =	!botan_pk_op_verify_finish(verify_op, signature.ptr, signature.len);

	botan_pk_op_verify_destroy(verify_op);
	return valid;
}

/**
 * Verification of an EMSA PSS signature described in PKCS#1
 */
static bool verify_emsa_pss_signature(private_botan_rsa_public_key_t *this,
									  rsa_pss_params_t *params, chunk_t data,
									  chunk_t signature)
{
	const char *hash;
	char hash_and_padding[BUF_LEN];

	if (!params)
	{
		return FALSE;
	}

	/* botan currently does not support passing the mgf1 hash */
	if (params->hash != params->mgf1_hash)
	{
		DBG1(DBG_LIB, "passing mgf1 hash not supported via botan");
		return FALSE;
	}

	hash = botan_get_hash(params->hash);
	if (!hash)
	{
		return FALSE;
	}

	if (params->salt_len > RSA_PSS_SALT_LEN_DEFAULT)
	{
		snprintf(hash_and_padding, sizeof(hash_and_padding),
				 "EMSA-PSS(%s,MGF1,%u)", hash, params->salt_len);
	}
	else
	{
		snprintf(hash_and_padding, sizeof(hash_and_padding),
				 "EMSA-PSS(%s,MGF1)", hash);
	}
	return verify_rsa_signature(this, hash_and_padding, data, signature);
}

METHOD(public_key_t, get_type, key_type_t,
	private_botan_rsa_public_key_t *this)
{
	return KEY_RSA;
}

METHOD(public_key_t, verify, bool,
	private_botan_rsa_public_key_t *this, signature_scheme_t scheme,
	void *params, chunk_t data, chunk_t signature)
{
	switch (scheme)
	{
		case SIGN_RSA_EMSA_PKCS1_NULL:
			return verify_rsa_signature(this, "EMSA_PKCS1(Raw)", data,
										signature);
		case SIGN_RSA_EMSA_PKCS1_SHA1:
			return verify_rsa_signature(this, "EMSA_PKCS1(SHA-1)", data,
										signature);
		case SIGN_RSA_EMSA_PKCS1_SHA2_224:
			return verify_rsa_signature(this, "EMSA_PKCS1(SHA-224)",
										data, signature);
		case SIGN_RSA_EMSA_PKCS1_SHA2_256:
			return verify_rsa_signature(this, "EMSA_PKCS1(SHA-256)",
										data, signature);
		case SIGN_RSA_EMSA_PKCS1_SHA2_384:
			return verify_rsa_signature(this, "EMSA_PKCS1(SHA-384)",
										data, signature);
		case SIGN_RSA_EMSA_PKCS1_SHA2_512:
			return verify_rsa_signature(this, "EMSA_PKCS1(SHA-512)",
										data, signature);
		case SIGN_RSA_EMSA_PSS:
			return verify_emsa_pss_signature(this, params, data, signature);
		default:
			DBG1(DBG_LIB, "signature scheme %N not supported via botan",
				 signature_scheme_names, scheme);
			return FALSE;
	}
}

METHOD(public_key_t, encrypt, bool,
	private_botan_rsa_public_key_t *this, encryption_scheme_t scheme,
	chunk_t plain, chunk_t *crypto)
{
	botan_pk_op_encrypt_t encrypt_op;
	botan_rng_t rng;
	const char* padding;

	switch (scheme)
	{
		case ENCRYPT_RSA_PKCS1:
			padding = "PKCS1v15";
			break;
		case ENCRYPT_RSA_OAEP_SHA1:
			padding = "OAEP(SHA-1)";
			break;
		case ENCRYPT_RSA_OAEP_SHA224:
			padding = "OAEP(SHA-224)";
			break;
		case ENCRYPT_RSA_OAEP_SHA256:
			padding = "OAEP(SHA-256)";
			break;
		case ENCRYPT_RSA_OAEP_SHA384:
			padding = "OAEP(SHA-384)";
			break;
		case ENCRYPT_RSA_OAEP_SHA512:
			padding = "OAEP(SHA-512)";
			break;
		default:
			DBG1(DBG_LIB, "encryption scheme %N not supported via botan",
				 encryption_scheme_names, scheme);
			return FALSE;
	}

	if (botan_rng_init(&rng, "user"))
	{
		return FALSE;
	}

	if (botan_pk_op_encrypt_create(&encrypt_op, this->key, padding, 0))
	{
		botan_rng_destroy(rng);
		return FALSE;
	}

	crypto->len = 0;
	if (botan_pk_op_encrypt_output_length(encrypt_op, plain.len, &crypto->len))
	{
		botan_rng_destroy(rng);
		botan_pk_op_encrypt_destroy(encrypt_op);
		return FALSE;
	}

	*crypto = chunk_alloc(crypto->len);
	if (botan_pk_op_encrypt(encrypt_op, rng, crypto->ptr, &crypto->len,
							plain.ptr, plain.len))
	{
		chunk_free(crypto);
		botan_rng_destroy(rng);
		botan_pk_op_encrypt_destroy(encrypt_op);
		return FALSE;
	}
	botan_rng_destroy(rng);
	botan_pk_op_encrypt_destroy(encrypt_op);
	return TRUE;
}

METHOD(public_key_t, get_keysize, int,
	private_botan_rsa_public_key_t *this)
{
	botan_mp_t n;
	size_t bits = 0;

	if (botan_mp_init(&n))
	{
		return 0;
	}

	if (botan_pubkey_rsa_get_n(n, this->key) ||
		botan_mp_num_bits(n, &bits))
	{
		botan_mp_destroy(n);
		return 0;
	}

	botan_mp_destroy(n);
	return bits;
}

METHOD(public_key_t, get_fingerprint, bool,
	private_botan_rsa_public_key_t *this, cred_encoding_type_t type,
	chunk_t *fp)
{
	return botan_get_fingerprint(this->key, this, type, fp);
}

METHOD(public_key_t, get_encoding, bool,
	private_botan_rsa_public_key_t *this, cred_encoding_type_t type,
	chunk_t *encoding)
{
	return botan_get_encoding(this->key, type, encoding);
}

METHOD(public_key_t, get_ref, public_key_t*,
	private_botan_rsa_public_key_t *this)
{
	ref_get(&this->ref);
	return &this->public.key;
}

METHOD(public_key_t, destroy, void,
	private_botan_rsa_public_key_t *this)
{
	if (ref_put(&this->ref))
	{
		lib->encoding->clear_cache(lib->encoding, this);
		botan_pubkey_destroy(this->key);
		free(this);
	}
}

/**
 * Internal generic constructor
 */
static private_botan_rsa_public_key_t *create_empty()
{
	private_botan_rsa_public_key_t *this;

	INIT(this,
		.public = {
			.key = {
				.get_type = _get_type,
				.verify = _verify,
				.encrypt = _encrypt,
				.equals = public_key_equals,
				.get_keysize = _get_keysize,
				.get_fingerprint = _get_fingerprint,
				.has_fingerprint = public_key_has_fingerprint,
				.get_encoding = _get_encoding,
				.get_ref = _get_ref,
				.destroy = _destroy,
			},
		},
		.ref = 1,
	);

	return this;
}

/*
 * Described in header
 */
botan_rsa_public_key_t *botan_rsa_public_key_adopt(botan_pubkey_t key)
{
	private_botan_rsa_public_key_t *this;

	this = create_empty();
	this->key = key;

	return &this->public;
}

/*
 * Described in header
 */
botan_rsa_public_key_t *botan_rsa_public_key_load(key_type_t type,
												  va_list args)
{
	private_botan_rsa_public_key_t *this = NULL;
	chunk_t n, e;

	n = e = chunk_empty;
	while (TRUE)
	{
		switch (va_arg(args, builder_part_t))
		{
			case BUILD_RSA_MODULUS:
				n = va_arg(args, chunk_t);
				continue;
			case BUILD_RSA_PUB_EXP:
				e = va_arg(args, chunk_t);
				continue;
			case BUILD_END:
				break;
			default:
				return NULL;
		}
		break;
	}

	if (n.ptr && e.ptr && type == KEY_RSA)
	{
		botan_mp_t mp_n, mp_e;

		if (!chunk_to_botan_mp(n, &mp_n))
		{
			return NULL;
		}

		if (!chunk_to_botan_mp(e, &mp_e))
		{
			botan_mp_destroy(mp_n);
			return NULL;
		}

		this = create_empty();

		if (botan_pubkey_load_rsa(&this->key, mp_n, mp_e))
		{
			botan_mp_destroy(mp_n);
			botan_mp_destroy(mp_e);
			free(this);
			return NULL;
		}

		botan_mp_destroy(mp_n);
		botan_mp_destroy(mp_e);
	}

	return &this->public;
}

#endif
