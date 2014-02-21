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
