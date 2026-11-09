/* gcc -Ibuild -Isrc src/strseg.test.c src/strseg.c -o build/strseg.test */
#include "config.h"
#include "daemonproxy.h"

int main() {
	int i;
	int64_t n;
	strseg_t s;
	const char *invalid[]= { "", "-", "9223372036854775808",
		"-9223372036854775809", "18446744073709551616", "99999999999999999999999999" };
	for (i= 0; i < sizeof(invalid)/sizeof(*invalid); i++) {
		s= STRSEG(invalid[i]);
		n= 42;
		assert(!strseg_atoi(&s, &n) && n == 42);
		assert(s.data == invalid[i] && s.len == strlen(invalid[i]));
	}
	s= STRSEG("9223372036854775807!");
	assert(strseg_atoi(&s, &n) && n == INT64_MAX && s.len == 1);
	s= STRSEG("-9223372036854775808");
	assert(strseg_atoi(&s, &n) && n == INT64_MIN && !s.len);
	s= STRSEG("-0");
	assert(strseg_atoi(&s, &n) && !n && !s.len);
	s= STRSEG("0");
	assert(strseg_atoi(&s, NULL) && !s.len);
	s= STRSEG("1TiB");
	assert(strseg_parse_size(&s, &n) && n == (1LL << 40) && !s.len);
	s= STRSEG("1TB");
	assert(strseg_parse_size(&s, &n) && n == 1000000000000LL && !s.len);
	s= STRSEG("8589934591GiB");
	assert(strseg_parse_size(&s, &n) && n == 9223372035781033984LL);
	s= STRSEG("8589934592GiB");
	assert(!strseg_parse_size(&s, &n));
	s= STRSEG("-1");
	assert(!strseg_parse_size(&s, &n));
	s= STRSEG("9223372036854775807B");
	assert(strseg_parse_size(&s, &n) && n == INT64_MAX);
	return 0;
}
