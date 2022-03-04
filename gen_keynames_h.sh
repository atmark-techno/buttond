#!/bin/sh

# GNU awk requires --non-decimal-data, but most don't..
AWK="awk"
if $AWK --non-decimal-data '' < /dev/null > /dev/null 2>&1; then
	AWK="$AWK --non-decimal-data"
fi


cat <<EOF
// SPDX-License-Identifier: MIT
// GENERATED FILE!

#ifndef BUTTOND_KEYNAMES_H
#define BUTTOND_KEYNAMES_H


EOF
$AWK '/^#define KEY_/ { $3=$3+0; if ($3) { keys[$3]=gensub(/KEY_/, "", 1, $2); }}
	END {
		printf("const char allkeynames[] =\n  \"");
		max = 0;
		for (key in keys) {
			key=key+0; # cast to int
			# 0x2ff is KEY_MAX, optimize for hole before it
			if (key > max && key < 0x2ff) {
				max = key;
			}
		}
		col = 2;
		for (i=1; i <= max; i++) {
			col+=length(keys[i])+4;
			if (col > 70) {
				printf("\"\n  \"");
				col = 2;
			}
			printf("%s\\000", keys[i]);
		}
		printf("\";");
	}' < /usr/include/linux/input-event-codes.h
cat <<EOF

#endif
EOF
