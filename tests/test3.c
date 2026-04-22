#include <stdio.h>
#include <stdlib.h>

#include "re.h"

int main(void) {
	int length = 0;
	int index = 0;

	re_t r = re_compile("\\d+");
	if (!r) {
		printf("FAIL: compile error\n");
	} else {
		index = re_matchp(r, "abc123def", &length);
		if (index != 3 || length != 3) {
			printf("FAIL: match 1 expected index=3,len=3 got index=%d,len=%d\n", index, length);
		}

		index = re_matchp(r, "42", &length);
		if (index != 0 || length != 2) {
			printf("FAIL: match 2 expected index=0,len=2 got index=%d,len=%d\n", index, length);
		}

		index = re_matchp(r, "assimoooo", &length);
		if (index != -1) {
			printf("FAIL: match 3 expected no match got index=%d\n", index);
		}

		free(r);
		printf("  PASS\n");
	}
}
