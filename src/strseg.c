/* strseg.c - routines for length-terminated string manipulation
 * Copyright (C) 2014  Michael Conrad
 * Distributed under GPLv2, see LICENSE
 */

#include "config.h"
#include "daemonproxy.h"

int strseg_cmp(strseg_t a, strseg_t b) {
	int i;
	for (i= 0; i < a.len && i < b.len; i++) {
		if (a.data[i] != b.data[i])
			return a.data[i] < b.data[i]? -1 : 1;
	}
	return a.len < b.len? -1
		: a.len > b.len? 1
		: 0;
}

/** Remove the next token from a delimited string.
 *
 * This function has slightly odd behavior.  It takes the first string and
 * consumes characters up to the first separator.  It assigns this range
 * (minus the separator) to the tok_out string, if given.
 *
 *  **If the input string is empty** an empty token will be returned, and the
 * input length **will be set to -1**.  This odd behavior handles the case
 * where the string "foo\tbar\t" actually has 3 fields, and the third is empty.
 * The consequence is that an empty string contains one empty token, and also
 * that the length of the string can go negative.  (it would perhaps be better
 * to use a NULL data pointer to indicate all tokens are consumed)
 */
bool strseg_tok_next(strseg_t *string_inout, char sep, strseg_t *tok_out) {
	const char *p= string_inout->data;
	int len;
	
	if (string_inout->len < 0)
		return false;
	
	for (len= 0; len < string_inout->len; len++)
		if (p[len] == sep)
			break;
	if (tok_out) {
		tok_out->data= p;
		tok_out->len= len;
	}
	string_inout->data+= len + 1;
	string_inout->len -= len + 1;
	return true;
}

/** Split a string on the next occurrence of a separator character
 *
 * Returns true if the separator character exists, in which case it
 * truncates the input string at that character and sets remainder_out
 * to the portion of the original string following the separator
 * (or the empty string if there is no separator)
 *
 * remainder_out is required.
 */
bool strseg_split_1(strseg_t *string_inout, char sep, strseg_t *remainder_out) {
	const char *p, *lim;
	for (p= string_inout->data, lim= p + string_inout->len; p < lim; p++) {
		if (*p == sep) {
			remainder_out->data= p + 1;
			remainder_out->len=  lim - remainder_out->data;
			string_inout->len=   p - string_inout->data;
			return true;
		}
	}
	remainder_out->len= 0;
	return false;
}

bool strseg_atoi(strseg_t *string, int64_t *val) {
	int64_t accum= 0;
	int i= 0;
	int sign= 1;

	if (string->len && string->data[0] == '-') {
		sign= -1;
		i++;
	}
	while (i < string->len && string->data[i] >= '0' && string->data[i] <= '9')
		accum= accum * 10 + (string->data[i++] - '0');
	if (i < (sign < 0? 2 : 1)) // did we get at least one digit?
		return false;
	string->data += i;
	string->len -= i;
	if (val) *val= accum * sign;
	return true;
}

/** Parse a positive integer with size suffix.
 *
 */
bool strseg_parse_size(strseg_t *string, int64_t *val) {
	long mul= 1, factor= 1024;
	int suffix_len;
	if (!strseg_atoi(string, val))
		return false;
	if (string->len < 1)
		return true; // return plain constant
	
	suffix_len= 1;
	if (string->len >= 3 && string->data[1] == 'i' && string->data[2] == 'B') {
		suffix_len= 3;
	}
	else if (string->len >= 2 && string->data[1] == 'B') {
		suffix_len= 2;
		factor= 1000; // SI units
	}
	
	switch (string->data[0]) {
	case 't': case 'T': mul= LONG_MAX; break;
	case 'g': case 'G': mul*= factor;
	case 'm': case 'M': mul*= factor;
	case 'k': case 'K': mul*= factor;
	case 'b': case 'B': break;
	default: return true;
	}

	// consume suffix
	string->data += suffix_len;
	string->len -= suffix_len;
	
	// make sure multiplied value fits in int64
	if (*val < 0 || *val * mul / mul != *val) {
		*val= (*val < 0)? -1 : 1;
		return false;
	}
	*val *= mul;
	return true;
}

bool strseg_parse_sockaddr(strseg_t *string, int addr_family, struct sockaddr_storage *a_out, int *len_out) {
	int64_t port_int;
	char name_buf[255];
	
	if (string->len <= 0) return false;
	
	if (addr_family == AF_UNIX) {
		struct sockaddr_un a;
		memset(&a, 0, sizeof(a));
		a.sun_family= AF_UNIX;
		
		if (string->len >= sizeof(a.sun_path))
			return false;
		memcpy(a.sun_path, string->data, string->len);
		a.sun_path[string->len]= '\0';
		
		// Save result to caller's variable
		if (a_out) memcpy(a_out, &a, sizeof(a));
		if (len_out) *len_out= sizeof(a);
		string->data+= string->len;
		string->len= 0;
		return true;
	}
	else if (addr_family == AF_INET) {
		struct sockaddr_in a;
		memset(&a, 0, sizeof(a));
		a.sin_family= AF_INET;
		
		if (string->len <= 0) return false;
		// '*' means INADDR_ANY
		if (string->data[0] == '*') {
			a.sin_addr.s_addr= INADDR_ANY;
			string->data++;
			string->len--;
		}
		else {
			const char *p, *lim;
			for (p= string->data, lim= p + string->len; p < lim; p++) {
				if (*p != '.' && (*p < '0' || *p > '9')) break;
			}
			int n= p - string->data;
			if (n <= 0 || n >= sizeof(name_buf))
				return false;
			memcpy(name_buf, string->data, n);
			name_buf[n]= '\0';
			if (inet_pton(addr_family, name_buf, &a.sin_addr) <= 0)
				return false;
			string->data+= n;
			string->len-= n;
		}
		// Check for :port notation
		if (string->len > 0 && string->data[0] == ':') {
			string->len--;
			string->data++;
			if (!strseg_atoi(string, &port_int))
				return false;
			a.sin_port= htons((short) port_int);
		}
		// Save result to caller's variable
		if (a_out) memcpy(a_out, &a, sizeof(a));
		if (len_out) *len_out= sizeof(a);
		return true;
	}
#if 0
	// TODO: correctly parse IPV6 addrs in the following formats:
	//   nn:nn::nn
	//   [nn:nn::nn]:port
	//   *:port
	else if (addr_family == AF_INET6) {
		struct sockaddr_in6 a;
		memset(&a, 0, sizeof(a));
		a.sin6_family= AF_INET6;

		// Check for addr:port notation
		addr= STRSEG("");
		...;
		// Convert address
		if (addr.len <= 0 || addr.len >= sizeof(name_buf))
			return false;
		memcpy(name_buf, addr.data, addr.len);
		name_buf[addr.len]= '\0';
		if (inet_pton(addr_family, name_buf, &a.sin6_addr) <= 0)
			// TODO: handle host names
			return false;
		
		// Save result to caller's variable
		if (a_out) memcpy(a_out, &a, sizeof(a));
		if (len_out) *len_out= sizeof(a);
		return true;
	}
#endif
	return false;
}
