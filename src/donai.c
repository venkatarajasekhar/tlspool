/* tlspool/localid.c -- Map the keys of local identities to credentials */

#include <config.h>

#include <stdlib.h>
#include <string.h>

#include <syslog.h>
#include <errno.h>

#include <gnutls/gnutls.h>
#include <gnutls/abstract.h>

#include <tlspool/internal.h>

#include "manage.h"
#include "donai.h"


/*
 * Lookup local identities from a BDB database.  The identities take the
 * form of a NAI, and are the keys for a key-values lookup.  The outcome
 * may offer multiple values, each representing an identity.  The general
 * structure of a value is:
 *
 * - 4 netbytes, a flags field for local identity management (see LID_xxx below)
 * - NUL-terminated string with a pkcs11 URI [ draft-pechanec-pkcs11uri ]
 * - Binary string holding the identity in binary form
 *
 * There may be prefixes for generic management, but these are not made
 * available to this layer.
 */



/* Retrieve flags from the credentials structure found in dbh_localid.
 * The function returns non-zero on success (zero indicates syntax error).
 */
int dbcred_flags (DBT *creddata, uint32_t *flags) {
	int p11privlen;
	if (creddata->size <= 4) {
		return 0;
	}
	*flags = ntohl (* (uint32_t *) creddata->data);
	return 1;
}


/* Interpret the credentials structure found in dbh_localid.
 * This comes down to splitting the (data,size) structure into fields:
 *  - a 32-bit flags field
 *  - a char * sharing the PKCS #11 private key location, NULL on LID_NO_PKCS11
 *  - a (data,size) structure for the public credential, also when LID_CHAINED
 * The function returns non-zero on success (zero indicates syntax error).
 */
int dbcred_interpret (gnutls_datum_t *creddata, uint32_t *flags, char **p11priv, uint8_t **pubdata, int *pubdatalen) {
	int p11privlen;
	if (creddata->size <= 4) {
		return 0;
	}
	*flags = ntohl (* (uint32_t *) creddata->data);
	if ((*flags) & LID_NO_PKCS11) {
		*p11priv = NULL;
	} else {
		*p11priv = ((char *) creddata->data) + 4;
		p11privlen = strnlen (*p11priv, creddata->size - 4);
		if (p11privlen == creddata->size - 4) {
			return 0;
		}
#ifdef TODO_PKCS11_ADDED
		if (strncmp (*p11priv, "pkcs11:", 7) != 0) {
			return 0;
		}
#endif
	}
	*pubdata    = ((uint8_t *) creddata->data) + 4 + p11privlen + 1;
	*pubdatalen =              creddata->size  - 4 - p11privlen - 1;
	if (*pubdatalen < 20) {
		// Unbelievably short certificate (arbitrary sanity limit 20)
		return 0;
	}
	return 1;
}


/* Create an iterator for a given localid value.  Use keys from dhb_lid.
 * The first value is delivered; continue with dbcred_iterate_next().
 *
 * The cursor must have been opened on dbh_localid within the desired
 * transaction context; the caller must close it after iteration.
 *
 * The value returned is only non-zero if a value was setup.
 * The DB_NOTFOUND value indicates that the key was not found.
 */
gtls_error dbcred_iterate_from_localid (DBC *cursor, DBT *keydata, DBT *creddata) {
	int gtls_errno = GNUTLS_E_SUCCESS;
	E_d2ge ("Key not found in db_localid",
		cursor->get (cursor, keydata, creddata, DB_SET));
	return gtls_errno;
}


/* Construct an iterator for a given remoteid selector.  Apply stepwise
 * generalisation to find the most concrete match.  The first value found
 * is delivered; continue with dbcred_iterate_next().
 *
 * The remotesel value in string representation is the key to discpatn,
 * forming the initial disclosure pattern.  This key should be setup with
 * enough space to store the pattern (which is never longer than the original
 * remoteid) plus a terminating NUL character.
 *
 * Note that remotesel already has the first value activated, usually the
 * same as the remoteid.  This is assumed to be available, so don't call
 * this function otherwise.  In practice, this is hardly a problem; any
 * valid remoteid will provide a valid selector whose first iteration is to
 * repeat the remoteid.  Failure to start even this is a sign of a syntax
 * error, which is good to be treating separately from not-found conditions.
 *
 * The started iteration is a nested iteration over dbh_disclose for the
 * pattern found, and inside that an iteration over dbh_localid for the
 * localid values that this gave.  This means that two cursors are needed,
 * both here and in the subsequent dbcred_iterate_next() calls.
 *
 * The cursors crs_disclose and crs_localid must have been opened on
 * dbh_disclose and dbh_localid within the desired transaction context;
 * the caller must close them after iteration.
 *
 * The value returned is zero if a value was setup; otherwise an error code.
 * The DB_NOTFOUND value indicates that no selector matching the remoteid
 * was found in dbh_disclose.
 */
gtls_error dbcred_iterate_from_remoteid_selector (DBC *crs_disclose, DBC *crs_localid, selector_t *remotesel, DBT *discpatn, DBT *keydata, DBT *creddata) {
	int gtls_errno = GNUTLS_E_SUCCESS;
	int more = 1;
	while (more) {
		int fnd;
		discpatn->size = donai_iterate_memput (discpatn->data, remotesel);
		tlog (TLOG_DB, LOG_DEBUG, "Looking up remote selector %.*s", discpatn->size, (char *) discpatn->data);
		fnd = crs_disclose->get (crs_disclose, discpatn, keydata, DB_SET);
		if (fnd == 0) {
			// Got the selector pattern!
			// Now continue, even when no localids will work.
			E_d2ge ("Key not found in db_localid",
				crs_localid->get (
					crs_localid,
					keydata,
					creddata,
					DB_SET));
			return gtls_errno;
		} else if (fnd != DB_NOTFOUND) {
			E_d2ge ("Failed while searching with remote ID selector", fnd);
			break;
		}
		more = selector_iterate_next (remotesel);
	}
	// Ended here with nothing more to find
	E_d2ge ("No selector matches remote ID in db_disclose",
		DB_NOTFOUND);
	return gtls_errno;
}


/* Move an iterator to the next credential data value.  When done, the value
 * returned should be DB_NOTFOUND.
 *
 * The outer cursor (for dbh_disclose) is optional, and is only used when
 * the prior call was from dbcred_iterate_from_remoteid().
 *
 * The optional discpatn must be supplied only when dbh_disclose is provided.
 * It holds the key value for the dbh_disclose outer cursor.
 *
 * The keydata will be filled with the intermediate key when dbh_disclose is
 * provided.  It is also used to match the next record with the current one.
 *
 * The value returned is zero if a value was setup; otherwise an error code.
 * The DB_NOTFOUND value indicates that no further duplicate was not found.
 */
db_error dbcred_iterate_next (DBC *opt_crs_disclose, DBC *crs_localid, DBT *opt_discpatn, DBT *keydata, DBT *creddata) {
	int db_errno = 0;
	db_errno = crs_localid->get (crs_localid, keydata, creddata, DB_NEXT_DUP);
	if (db_errno != DB_NOTFOUND) {
		return db_errno;
	}
	// Inner loop ended in DB_NOTFOUND, optionally continue in outer loop
	if ((opt_crs_disclose != NULL) && (opt_discpatn != NULL)) {
		while (db_errno == DB_NOTFOUND) {
			db_errno = opt_crs_disclose->get (opt_crs_disclose, opt_discpatn, keydata, DB_NEXT_DUP);
			if (db_errno == DB_NOTFOUND) {
				return db_errno;
			}
			db_errno = crs_localid->get (crs_localid, keydata, creddata, DB_SET);
		}
	}
	return db_errno;
}


/* Iterate over selector values that would generalise the donai.  The
 * selector_t shares data from the donai, so it allocates no internal
 * storage and so it can be dropped at any time during the iteration.
 * Meanwhile, the donai must not drop storage before iteration stops.
 *
 * The value returned is only non-zero if a value was setup.
 */
int selector_iterate_init (selector_t *iterator, donai_t *donai) {
	//
	// If the user name is not NULL but empty, bail out in horror
	if ((donai->user != NULL) && (donai->userlen <= 0)) {
		return 0;
	}
	//
	// If the domain name is empty or NULL, bail out in horror
	if ((donai->domain == NULL) || (donai->domlen == 0)) {
		return 0;
	}
	//
	// The first and most concrete pattern is the donai itself
	memcpy (iterator, donai, sizeof (*iterator));
	return 1;
}

int selector_iterate_next (selector_t *iterator) {
	int skip;
	//
	// If the user name is not NULL but empty, bail out in horror
	if ((iterator->user != NULL) && (iterator->userlen == 0)) {
		return 0;
	}
	//
	// If the domain name is empty or NULL, bail out in horror
	if ((iterator->domain == NULL) || (iterator->domlen == 0)) {
		return 0;
	}
	//
	// If there is a user component and it is non-empty, make it empty
	// If it was empty, permit it to become non-empty again, and continue
	if (iterator->user) {
		if (iterator->userlen > 0) {
			iterator->userlen = -iterator->userlen;
			return 1;
		}
		iterator->userlen = -iterator->userlen;
	}
	//
	// If the domain is a single dot, we're done
	if ((iterator->domlen == 1) && (*iterator->domain == '.')) {
		return 0;
	}
	//
	// Replace the domain (known >= 1 chars) with the next dot's domain
	skip = 1;
	while ((skip < iterator->domlen) && (iterator->domain [skip] != '.')) {
		skip++;
	}
	if (skip == iterator->domlen) {
		iterator->domain = ".";		// Last resort domain
		iterator->domlen = 1;
	} else {
		iterator->domain += skip;
		iterator->domlen -= skip;
	}
	return 1;
}


/* Check if a selector is a pattern that matches the given donai value.
 * The value returned is non-zero for a match, zero for a non-match.
 */
int donai_matches_selector (donai_t *donai, selector_t *pattern) {
	int extra;
	//
	// Bail out in horror on misconfigurations
	if ((donai->user != NULL) && (donai->userlen <= 0)) {
		return 0;
	}
	if ((donai  ->domain == NULL) || (donai  ->domlen <= 0)) {
		return 0;
	}
	if ((pattern->domain == NULL) || (pattern->domlen <= 0)) {
		return 0;
	}
	//
	// User name handling first
	if (pattern->user) {
		//
		// Pattern has a user?  Then request a user in the donai too
		if (donai->user == NULL) {
			return 0;
		}
		//
		// Non-empty user in pattern?  Then match everything
		if (*pattern->user) {
			if (pattern->userlen > 0) {
				if (donai->userlen != pattern->userlen) {
					return 0;
				}
				if (memcmp (donai->user, pattern->user, donai->userlen) != 0) {
					return 0;
				}
			}
		}
	} else {
		//
		// Pattern without user, then donai may not have one either
		if (donai->user != NULL) {
			return 0;
		}
	}
	//
	// Domain name handling second
	if (*pattern->domain == '.') {
		extra = donai->domlen - pattern->domlen;
		if (extra < 0) {
			//
			// No good having a longer pattern than a donai.domain
			return 0;
		}
	} else {
		extra = 0;
	}
	return (memcmp (donai->domain + extra, pattern->domain, pattern->domlen) == 0);
}


/* Fill a donai structure from a stable string. The donai will share parts
 * of the string.  The function can also be used to construct a selector
 * from a string; their structures are the same and the syntax is not
 * parsed to ensure non-empty usernames and non-dot-prefixed domain names.
 */
donai_t donai_from_stable_string (char *stable, int stablelen) {
	donai_t retval;
	retval.userlen = stablelen - 1;
	while (retval.userlen > 0) {
		if (stable [retval.userlen] == '@') {
			break;
		}
		retval.userlen--;
	}
	if (stable [retval.userlen] == '@') {
		retval.user = stable;
		retval.domain = stable + (retval.userlen + 1);
		retval.domlen = stablelen - 1 - retval.userlen;
	} else {
		retval.user = NULL;
		retval.domain = stable;
		retval.domlen = stablelen;
	}
	return retval;
}

/* Print a donai or iterated selector to the given text buffer.  The
 * text will be precisely the same as the originally parsed text.  An
 * iterator may deliver values that are shorter, not longer.  The value
 * returned is the number of bytes written.  No trailing NUL character
 * will be written.
 */
int donai_iterate_memput (char *selector_text, donai_t *iterator) {
	int len = 0;
	if (iterator->user != NULL) {
		if (iterator->userlen > 0) {
			memcpy (selector_text, iterator->user, iterator->userlen);
			len += iterator->userlen;
		}
		selector_text [len++] = '@';
	}
	memcpy (selector_text + len, iterator->domain, iterator->domlen);
	len += iterator->domlen;
	return len;
}
